#define _GNU_SOURCE

#include "server.h"
#include "daemon.h"
#include "bhist.config.h"
#include "version.h"
#include "protocol.h"
#include "network.h"
#include "lproto.h"

#include <sqlite3.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Groups requests by scheduling cost for queueing and performance metrics. */
enum requestClass {
    REQUEST_CLASS_JOB_ID = 0,
    REQUEST_CLASS_TIME_RANGE = 1,
    REQUEST_CLASS_HISTORY_SCAN = 2,
    REQUEST_CLASS_TOTAL = 3,
    REQUEST_CLASS_COUNT = 4
};

/* Stores last-period, peak-period, and lifetime values for one metric. */
struct metricValues {
    uint64_t last;
    uint64_t max;
    uint64_t total;
};

/* Immutable copy of completed-period request and output counters. */
struct metricsSnapshot {
    uint64_t completePeriodCount;
    struct metricValues requests[REQUEST_CLASS_COUNT];
    struct metricValues output;
};

/* Owns the live and completed-period counters maintained by the server. */
struct metrics {
    int enabled;
    uint64_t requestCurrent[REQUEST_CLASS_COUNT];
    uint64_t outputCurrent;
    uint64_t requestLast[REQUEST_CLASS_COUNT];
    uint64_t requestMax[REQUEST_CLASS_COUNT];
    uint64_t requestTotal[REQUEST_CLASS_COUNT];
    uint64_t outputLast;
    uint64_t outputMax;
    uint64_t outputTotal;
    uint64_t completePeriodCount;
};

/*
 * Read wall-clock time in milliseconds for request timing and deadlines.
 * @return: Milliseconds since the Unix epoch, or 0 on failure.
 */
static long long
nowMillis(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (long long)tv.tv_sec * 1000LL +
        (long long)tv.tv_usec / 1000LL;
}

/*
 * bhist-speedup TCP service and bhist-speedup-server management commands.
 *
 * The daemon parent accepts connections, validates protocol frames, maintains
 * the priority queue, enforces concurrency limits, and samples perfmon. Each
 * running request forks a handler, which forks the query function and encodes
 * its standard output, standard error, and exit state as protocol frames. A
 * handler leads a process group containing the query and
 * all bhist descendants, so disconnect and shutdown cleanup signals the group.
 *
 * The parent never opens SQLite or retains query state. Its queue is a bounded
 * singly linked list. Scan requests obey a separate limit; point job-ID requests
 * may start immediately, while every handler still counts toward the total
 * limit. Queued job IDs are inserted at the head and scans at the FIFO tail.
 *
 * Perfmon is disabled by default and never persisted. Once started, the main
 * loop samples at fixed monotonic boundaries; view returns only the most recent
 * complete period and never triggers an ad hoc sample.
 */

#define SERVICE_PEER_MAX 160
#define SERVICE_ERROR_MAX 1024
#define SERVICE_SHUTDOWN_GRACE_MS 5000
#define HANDLER_CANCEL_GRACE_MS 2000
#define SERVICE_LOG_DETAIL_MAX 3072
#define SERVICE_LOG_LINE_MAX 4096

/* Owns the copied server configuration across daemonStart's fork boundary. */
struct daemonContext {
    struct config config;
};

/* Borrows the endpoint and owns concurrency and timeout limits for the daemon. */
struct serviceConfig {
    const char *endpoint;
    int maxRunning;
    int maxScanRunning;
    int maxQueued;
    int requestTimeoutSeconds;
};

/* Owns a queued client connection, decoded request, and linked-list position. */
struct pendingRequest {
    int fd;
    unsigned long long requestId;
    long long enqueuedAt;
    char peer[SERVICE_PEER_MAX];
    enum requestClass requestClass;
    struct queryRequest query;
    struct pendingRequest *next;
};

/* Shares a lock-free output-byte counter between a handler and server parent. */
struct handlerMetrics {
    unsigned long long outputBytes;
};

/* Tracks one running handler process and its sampled output counter. */
struct activeHandler {
    pid_t pid;
    unsigned long long requestId;
    enum requestClass requestClass;
    struct handlerMetrics *metrics;
    uint64_t collectedOutputBytes;
    struct activeHandler *next;
};

/* Owns the active perfmon schedule and last fixed-period sample boundary. */
struct servicePerformanceMonitor {
    int enabled;
    unsigned periodSeconds;
    long long nextSampleMs;
    uint64_t elapsedPeriods;
    uint64_t monitorStartTime;
    uint64_t lastSampleEndTime;
};

/* Set by parentStopSignal when service shutdown is requested. */
static volatile sig_atomic_t parentStopRequested;
/* Set by parentChildSignal when one or more handlers may be reapable. */
static volatile sig_atomic_t parentChildChanged;
/* Set in a handler when termination or client-disconnect cleanup begins. */
static volatile sig_atomic_t handlerStopRequested;
/* Handler-owned client descriptor closed by the asynchronous stop path. */
static int handlerClientFd = -1;
/* Lifetime and current-period request/output counters owned by the parent. */
static struct metrics serviceMetrics;
/* Parent-owned fixed-period perfmon schedule. */
static struct servicePerformanceMonitor serviceMonitor;

static long long monotonicMillis(void);
static void serviceLog(const char *event, unsigned long long requestId,
                       const char *format, ...);
static void writeU32(unsigned char *buffer, uint32_t value);
static uint32_t readU32(const unsigned char *buffer);
static int sendErrorFrame(int fd, uint32_t code, const char *message);
static int sendStatusFrame(int fd, int running, int queued,
                           const struct serviceConfig *config);
static int sendPerformanceFrame(int fd);
static int validateUserHint(const char *userHint);
static void describePeer(int fd, char *buffer, size_t bufferLen);
static void clearRecvTimeout(int fd);
static void parentStopSignal(int signalNumber);
static void parentChildSignal(int signalNumber);
static void handlerStopSignal(int signalNumber);
static void installParentSignals(void);
static void installHandlerSignals(void);
static void freePendingRequest(struct pendingRequest *request);
static void closeQueuedFds(const struct pendingRequest *head, int keepFd);
static char *formatQueryCommand(const struct queryRequest *query);
static void recordQueryHistory(const struct pendingRequest *pending,
                               long long completedAtMs,
                               long long queueMs,
                               long long executionMs,
                               int exitStatus);
static int relayQuery(struct pendingRequest *pending,
                      struct handlerMetrics *metrics);
static int startRequest(struct pendingRequest *pending,
                        int listenerFd, const struct pendingRequest *queue,
                        struct activeHandler **activeHead,
                        int *activeCount);
static void appendQueue(struct pendingRequest **head,
                        struct pendingRequest **tail,
                        struct pendingRequest *request);
static struct pendingRequest *popQueue(struct pendingRequest **head,
                                        struct pendingRequest **tail);
static int countActiveScans(const struct activeHandler *activeHead);
static void removeQueuedRequest(struct pendingRequest **head,
                                struct pendingRequest **tail,
                                struct pendingRequest *target,
                                int *queuedCount);
static void dispatchQueue(const struct serviceConfig *config,
                          struct pendingRequest **queueHead,
                          struct pendingRequest **queueTail,
                          int *queuedCount, int listenerFd,
                          struct activeHandler **activeHead,
                          int *activeCount);
static void reapHandlers(struct activeHandler **activeHead,
                         int *activeCount);
static void collectActiveOutput(struct activeHandler *activeHead);
static int startPerformanceMonitor(unsigned periodSeconds,
                                   struct activeHandler *activeHead,
                                   char *error, size_t errorSize);
static void stopPerformanceMonitor(void);
static void samplePerformanceIfDue(struct activeHandler *activeHead);
static int performancePollTimeout(void);
static int receiveConnection(const struct serviceConfig *config,
                             int listenerFd,
                             struct pendingRequest **queueHead,
                             struct pendingRequest **queueTail,
                             int *queuedCount,
                             struct activeHandler **activeHead,
                             int *activeCount,
                             unsigned long long *nextRequestId);
static void watchQueuedClients(int listenerFd,
                               struct pendingRequest **queueHead,
                               struct pendingRequest **queueTail,
                               int *queuedCount);
static void shutdownService(int listenerFd,
                            struct pendingRequest **queueHead,
                            struct pendingRequest **queueTail,
                            int *queuedCount,
                            struct activeHandler **activeHead,
                            int *activeCount);

/* In-memory fixed-period metrics owned only by the server main loop. */

/*
 * Add two counters without allowing unsigned wraparound.
 * @param[in] left: Existing counter value.
 * @param[in] right: Increment to add.
 * @return: Sum of both values, saturated at UINT64_MAX.
 */
static uint64_t
saturatingAdd(uint64_t left, uint64_t right)
{
    if (UINT64_MAX - left < right)
        return UINT64_MAX;
    return left + right;
}

/*
 * Initialize metric state.
 * @param[out] metrics: Metric state to initialize.
 * @return: None.
 */
static void
metricsInit(struct metrics *metrics)
{
    memset(metrics, 0, sizeof(*metrics));
}

/*
 * Start metric sampling and reset previous values.
 * @param[in,out] metrics: Metric state to start.
 * @return: None.
 */
static void
metricsStart(struct metrics *metrics)
{
    if (metrics == NULL)
        return;
    memset(metrics, 0, sizeof(*metrics));
    metrics->enabled = 1;
}

/*
 * Stop metric sampling and clear all values.
 * @param[in,out] metrics: Metric state to stop.
 * @return: None.
 */
static void
metricsStop(struct metrics *metrics)
{
    if (metrics == NULL)
        return;
    memset(metrics, 0, sizeof(*metrics));
}

/*
 * Test whether metric sampling is enabled.
 * @param[in] metrics: Metric state to inspect.
 * @return: 1 when enabled, otherwise 0.
 */
static int
metricsIsEnabled(const struct metrics *metrics)
{
    return metrics != NULL && metrics->enabled;
}

/*
 * Complete the current sample period and roll accumulated values forward.
 * @param[in,out] metrics: Metric state to update.
 * @return: None.
 */
static void
metricsCompletePeriod(struct metrics *metrics)
{
    int requestClass;

    if (!metricsIsEnabled(metrics))
        return;
    for (requestClass = 0;
         requestClass < REQUEST_CLASS_COUNT; requestClass++) {
        uint64_t current;

        current = metrics->requestCurrent[requestClass];
        metrics->requestLast[requestClass] = current;
        if (metrics->completePeriodCount == 0 ||
            current > metrics->requestMax[requestClass])
            metrics->requestMax[requestClass] = current;
        metrics->requestTotal[requestClass] = saturatingAdd(
            metrics->requestTotal[requestClass], current);
    }
    if (metrics->completePeriodCount == 0 ||
        metrics->outputCurrent > metrics->outputMax)
        metrics->outputMax = metrics->outputCurrent;
    metrics->outputLast = metrics->outputCurrent;
    metrics->outputTotal = saturatingAdd(
        metrics->outputTotal, metrics->outputCurrent);
    metrics->completePeriodCount = saturatingAdd(
        metrics->completePeriodCount, 1);
    memset(metrics->requestCurrent, 0,
           sizeof(metrics->requestCurrent));
    metrics->outputCurrent = 0;
}

/*
 * Record one query request.
 * @param[in,out] metrics: Metric state to update.
 * @param[in] requestClass: Query request class.
 * @return: None.
 */
static void
metricsRecordRequest(struct metrics *metrics,
                     enum requestClass requestClass)
{
    if (!metricsIsEnabled(metrics) ||
        requestClass < REQUEST_CLASS_JOB_ID ||
        requestClass >= REQUEST_CLASS_TOTAL)
        return;
    metrics->requestCurrent[requestClass] = saturatingAdd(
        metrics->requestCurrent[requestClass], 1);
    metrics->requestCurrent[REQUEST_CLASS_TOTAL] = saturatingAdd(
        metrics->requestCurrent[REQUEST_CLASS_TOTAL], 1);
}

/*
 * Record bytes sent to clients.
 * @param[in,out] metrics: Metric state to update.
 * @param[in] bytes: Additional output bytes.
 * @return: None.
 */
static void
metricsRecordOutput(struct metrics *metrics, uint64_t bytes)
{
    if (!metricsIsEnabled(metrics) || bytes == 0)
        return;
    metrics->outputCurrent = saturatingAdd(metrics->outputCurrent, bytes);
}

/*
 * Copy metrics for completed sample periods.
 * @param[in] metrics: Metric state to read.
 * @param[out] snapshot: Snapshot receiving completed-period values.
 * @return: None.
 */
static void
metricsSnapshot(const struct metrics *metrics,
                struct metricsSnapshot *snapshot)
{
    int requestClass;

    memset(snapshot, 0, sizeof(*snapshot));
    if (metrics == NULL)
        return;
    snapshot->completePeriodCount = metrics->completePeriodCount;
    for (requestClass = 0;
         requestClass < REQUEST_CLASS_COUNT; requestClass++) {
        snapshot->requests[requestClass].last =
            metrics->requestLast[requestClass];
        snapshot->requests[requestClass].max =
            metrics->requestMax[requestClass];
        snapshot->requests[requestClass].total =
            metrics->requestTotal[requestClass];
    }
    snapshot->output.last = metrics->outputLast;
    snapshot->output.max = metrics->outputMax;
    snapshot->output.total = metrics->outputTotal;
}

/*
 * Test whether a supported bhist short option consumes a value.
 * @param[in] option: Short option without its leading dash.
 * @return: Nonzero when the option consumes a value, otherwise zero.
 */
static int
optionNeedsValue(char option)
{
    /*
     * Keep this list synchronized with runQuery() getopt handling. Missing a
     * value-taking option causes its separate value to be mistaken for a job
     * selector, incorrectly granting job-ID queue priority and corrupting the
     * perfmon request classification.
     */
    return option == 'u' || option == 'q' || option == 'm' ||
        option == 'S' || option == 'D' || option == 'C';
}

/*
 * Classify a query from its command-line arguments.
 * @param[in] argc: Number of arguments.
 * @param[in] argv: Argument vector.
 * @return: Query request class.
 */
static enum requestClass
classifyRequestArgs(int argc, char *const argv[])
{
    int hasSelector;
    int hasTimeRange;
    int endOptions;
    int i;

    hasSelector = 0;
    hasTimeRange = 0;
    endOptions = 0;
    /*
     * This function classifies scheduling and metrics only; runQuery() remains
     * authoritative for validation. A new time-range option must set
     * hasTimeRange alongside -S, -D, and -C below.
     */
    for (i = 0; i < argc; i++) {
        const char *argument;
        size_t optionIndex;

        argument = argv[i];
        if (argument == NULL)
            continue;
        if (endOptions || argument[0] != '-' || argument[1] == '\0') {
            hasSelector = 1;
            continue;
        }
        if (strcmp(argument, "--") == 0) {
            endOptions = 1;
            continue;
        }
        if (argument[1] == '-')
            continue;

        optionIndex = 1;
        while (argument[optionIndex] != '\0') {
            char option;

            option = argument[optionIndex++];
            if (option == 'S' || option == 'D' || option == 'C')
                hasTimeRange = 1;
            if (!optionNeedsValue(option))
                continue;
            if (argument[optionIndex] == '\0' && i + 1 < argc)
                i++;
            break;
        }
    }

    if (hasSelector)
        return REQUEST_CLASS_JOB_ID;
    if (hasTimeRange)
        return REQUEST_CLASS_TIME_RANGE;
    return REQUEST_CLASS_HISTORY_SCAN;
}

/*
 * Read monotonic time for queue delays and fixed perfmon boundaries.
 * @return: Monotonic milliseconds, or zero on failure.
 */
static long long
monotonicMillis(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (long long)value.tv_sec * 1000LL +
        (long long)value.tv_nsec / 1000000LL;
}

/*
 * Emit one structured server lifecycle or request log record.
 * @param[in] event: Stable event name.
 * @param[in] requestId: Request ID, or zero for a service event.
 * @param[in] format: Optional printf-style detail format and arguments.
 * @return: None; caller errno is preserved.
 */
static void
serviceLog(const char *event, unsigned long long requestId,
           const char *format, ...)
{
    char detail[SERVICE_LOG_DETAIL_MAX];
    char line[SERVICE_LOG_LINE_MAX];
    va_list arguments;
    long long logStarted;
    int savedErrno;

    savedErrno = errno;
    detail[0] = '\0';
    if (format != NULL && format[0] != '\0') {
        va_start(arguments, format);
        (void)vsnprintf(detail, sizeof(detail), format, arguments);
        va_end(arguments);
    }
    logStarted = nowMillis();
    if (requestId != 0) {
        (void)snprintf(
            line, sizeof(line),
            "bhist_speedup_server time_ms=%lld pid=%ld event=%s "
            "request_id=%llu%s%s",
            logStarted, (long)getpid(), event, requestId,
            detail[0] != '\0' ? " " : "", detail);
    } else {
        (void)snprintf(
            line, sizeof(line),
            "bhist_speedup_server time_ms=%lld pid=%ld event=%s%s%s",
            logStarted, (long)getpid(), event,
            detail[0] != '\0' ? " " : "", detail);
    }
    ls_syslog(LOG_INFO, "%s", line);
    errno = savedErrno;
}

/*
 * Encode one 32-bit integer in network byte order.
 * @param[out] buffer: Four-byte destination.
 * @param[in] value: Host-order value.
 * @return: None.
 */
static void
writeU32(unsigned char *buffer, uint32_t value)
{
    uint32_t networkValue;

    networkValue = htonl(value);
    memcpy(buffer, &networkValue, sizeof(networkValue));
}

/*
 * Decode one 32-bit network-order integer.
 * @param[in] buffer: Four-byte source.
 * @return: Host-order value.
 */
static uint32_t
readU32(const unsigned char *buffer)
{
    uint32_t value;

    memcpy(&value, buffer, sizeof(value));
    return ntohl(value);
}

/*
 * Send a bounded textual protocol error to one client.
 * @param[in] fd: Client socket.
 * @param[in] code: Stable protocol error code.
 * @param[in] message: Human-readable error text.
 * @return: 0 on success, otherwise -1.
 */
static int
sendErrorFrame(int fd, uint32_t code, const char *message)
{
    unsigned char payload[SERVICE_ERROR_MAX + 4];
    size_t messageLength;

    if (message == NULL)
        message = "unknown service error";
    messageLength = strlen(message);
    if (messageLength > SERVICE_ERROR_MAX)
        messageLength = SERVICE_ERROR_MAX;
    writeU32(payload, code);
    memcpy(payload + 4, message, messageLength);
    return sendFrame(fd, FRAME_ERROR, payload,
                          (uint32_t)messageLength + 4);
}

/*
 * Send current load and configured concurrency limits to a status client.
 * @param[in] fd: Client socket.
 * @param[in] running: Number of running handlers.
 * @param[in] queued: Number of queued requests.
 * @param[in] config: Service concurrency limits.
 * @return: 0 on success, otherwise -1.
 */
static int
sendStatusFrame(int fd, int running, int queued,
                const struct serviceConfig *config)
{
    unsigned char payload[20];

    writeU32(payload, (uint32_t)running);
    writeU32(payload + 4, (uint32_t)queued);
    writeU32(payload + 8, (uint32_t)config->maxRunning);
    writeU32(payload + 12, (uint32_t)config->maxScanRunning);
    writeU32(payload + 16, (uint32_t)config->maxQueued);
    return sendFrame(fd, FRAME_STATUS_RESULT, payload,
                          sizeof(payload));
}

/*
 * Send the current perfmon enabled state after a control request.
 * @param[in] fd: Client socket.
 * @return: 0 on success, otherwise -1.
 */
static int
sendPerfmonResult(int fd)
{
    unsigned char payload[4];

    writeU32(payload, serviceMonitor.enabled ? 1U : 0U);
    return sendFrame(fd, FRAME_PERFMON_RESULT,
                          payload, sizeof(payload));
}

/*
 * Copy one internal metric triple into its wire representation.
 * @param[out] destination: Protocol metric destination.
 * @param[in] source: Internal metric source.
 * @return: None.
 */
static void
copyMetricValues(struct performanceMetric *destination,
                 const struct metricValues *source)
{
    destination->last = source->last;
    destination->max = source->max;
    destination->total = source->total;
}

/*
 * Encode and send the latest completed performance-monitor snapshot.
 * @param[in] fd: Client socket.
 * @return: 0 on success, otherwise -1.
 */
static int
sendPerformanceFrame(int fd)
{
    struct metricsSnapshot snapshot;
    struct performanceResult result;
    unsigned char *payload;
    uint32_t payloadLength;
    int i;
    int rc;

    metricsSnapshot(&serviceMetrics, &snapshot);
    memset(&result, 0, sizeof(result));
    result.monitorStarted = serviceMonitor.enabled ? 1U : 0U;
    if (serviceMonitor.enabled) {
        long long nowMs;
        long long remainingMs;

        result.samplePeriodSeconds = serviceMonitor.periodSeconds;
        result.monitorStartTime = serviceMonitor.monitorStartTime;
        result.lastSampleEndTime = serviceMonitor.lastSampleEndTime;
        nowMs = monotonicMillis();
        remainingMs = serviceMonitor.nextSampleMs - nowMs;
        if (snapshot.completePeriodCount == 0 && remainingMs > 0)
            result.secondsUntilFirstSample =
                (uint32_t)((remainingMs + 999) / 1000);
    }
    result.completePeriodCount = snapshot.completePeriodCount;
    for (i = 0; i < REQUEST_CLASS_COUNT; i++)
        copyMetricValues(&result.requests[i], &snapshot.requests[i]);
    copyMetricValues(&result.output, &snapshot.output);

    payload = NULL;
    payloadLength = 0;
    if (encodePerformanceResult(&result, &payload,
                                      &payloadLength) != 0)
        return -1;
    rc = sendFrame(fd, FRAME_PERFORMANCE_RESULT,
                        payload, payloadLength);
    free(payload);
    return rc;
}

/*
 * Validate the printable client-supplied user hint used only in logs/env.
 * @param[in] userHint: Claimed local user name from the query client.
 * @return: 0 when valid, otherwise -1.
 */
static int
validateUserHint(const char *userHint)
{
    const unsigned char *cursor;

    if (userHint == NULL || userHint[0] == '\0' ||
        strlen(userHint) > PROTOCOL_USER_HINT_MAX)
        return -1;
    cursor = (const unsigned char *)userHint;
    while (*cursor != '\0') {
        if (*cursor < 32 || *cursor == 127)
            return -1;
        cursor++;
    }
    return 0;
}

/*
 * Format the numeric address and port of a connected peer.
 * @param[in] fd: Connected client socket.
 * @param[out] buffer: Buffer receiving host:port or "unknown".
 * @param[in] bufferLen: Size of buffer.
 * @return: None.
 */
static void
describePeer(int fd, char *buffer, size_t bufferLen)
{
    struct sockaddr_storage address;
    socklen_t addressLength;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];

    addressLength = sizeof(address);
    if (getpeername(fd, (struct sockaddr *)&address, &addressLength) != 0 ||
        getnameinfo((struct sockaddr *)&address, addressLength,
                    host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(buffer, bufferLen, "unknown");
        return;
    }
    snprintf(buffer, bufferLen, "%.128s:%.16s", host, service);
}

/*
 * Remove the temporary receive timeout after request framing completes.
 * @param[in] fd: Connected client socket.
 * @return: None.
 */
static void
clearRecvTimeout(int fd)
{
    struct timeval timeout;

    memset(&timeout, 0, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
}

/*
 * Record SIGTERM or SIGINT for the server parent event loop.
 * @param[in] signalNumber: Delivered signal number.
 * @return: None.
 */
static void
parentStopSignal(int signalNumber)
{
    (void)signalNumber;
    parentStopRequested = 1;
}

/*
 * Record SIGCHLD so the parent event loop reaps completed handlers.
 * @param[in] signalNumber: Delivered signal number.
 * @return: None.
 */
static void
parentChildSignal(int signalNumber)
{
    (void)signalNumber;
    parentChildChanged = 1;
}

/*
 * Interrupt a handler and wake any blocking client-socket operation.
 * @param[in] signalNumber: Delivered signal number.
 * @return: None.
 */
static void
handlerStopSignal(int signalNumber)
{
    (void)signalNumber;
    handlerStopRequested = 1;
    if (handlerClientFd >= 0)
        (void)shutdown(handlerClientFd, SHUT_RDWR);
}

/*
 * Install parent shutdown and child-state signal handlers.
 * @return: None.
 */
static void
installParentSignals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = parentStopSignal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = parentChildSignal;
    sigaction(SIGCHLD, &action, NULL);
}

/*
 * Install handler shutdown signals and restore default SIGCHLD behavior.
 * @return: None.
 */
static void
installHandlerSignals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = handlerStopSignal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGCHLD, SIG_DFL);
}

/*
 * Close and release one pending request and its decoded argument storage.
 * @param[in,out] request: Pending request to release.
 * @return: None.
 */
static void
freePendingRequest(struct pendingRequest *request)
{
    if (request == NULL)
        return;
    if (request->fd >= 0)
        close(request->fd);
    freeQueryRequest(&request->query);
    free(request);
}

/*
 * Close inherited queued client descriptors except one handler-owned socket.
 * @param[in] head: Queue head visible after fork.
 * @param[in] keepFd: Descriptor retained by the handler child.
 * @return: None.
 */
static void
closeQueuedFds(const struct pendingRequest *head, int keepFd)
{
    while (head != NULL) {
        if (head->fd >= 0 && head->fd != keepFd)
            close(head->fd);
        head = head->next;
    }
}

/*
 * Join the received bhist argument vector for request-history inspection.
 * @param[in] query: Decoded request arguments.
 * @return: Newly allocated command text, otherwise NULL.
 */
static char *
formatQueryCommand(const struct queryRequest *query)
{
    char *command;
    char *cursor;
    size_t length;
    size_t argumentLength;
    int i;

    length = strlen("bhist");
    for (i = 0; i < query->argc; i++) {
        argumentLength = strlen(query->argv[i]);
        if (length > SIZE_MAX - argumentLength - 1)
            return NULL;
        length += argumentLength + 1;
    }
    command = (char *)malloc(length + 1);
    if (command == NULL)
        return NULL;
    cursor = command;
    memcpy(cursor, "bhist", strlen("bhist"));
    cursor += strlen("bhist");
    for (i = 0; i < query->argc; i++) {
        *cursor++ = ' ';
        argumentLength = strlen(query->argv[i]);
        memcpy(cursor, query->argv[i], argumentLength);
        cursor += argumentLength;
    }
    *cursor = '\0';
    return command;
}

/*
 * Append one completed request to the schema-3 history table.
 * @param[in] pending: Request identity and original bhist arguments.
 * @param[in] completedAtMs: Completion wall-clock time in Unix milliseconds.
 * @param[in] queueMs: Time spent waiting before query execution.
 * @param[in] executionMs: Query execution time, excluding this insert.
 * @param[in] exitStatus: Shell-style query exit status.
 * @return: None; failures are logged and never change the client result.
 */
static void
recordQueryHistory(const struct pendingRequest *pending,
                   long long completedAtMs, long long queueMs,
                   long long executionMs, int exitStatus)
{
    static const char insertSql[] =
        "INSERT INTO bhist_request_history "
        "(completed_at_ms, request_user, command, queue_ms, "
        "execution_ms, exit_status) VALUES (?, ?, ?, ?, ?, ?)";
    const char *dbPath;
    const char *busyTimeout;
    sqlite3_stmt *statement;
    sqlite3 *db;
    char *command;
    int rc;

    dbPath = getenv("LSB_BHIST_INTERNAL_DB_PATH");
    command = formatQueryCommand(&pending->query);
    db = NULL;
    statement = NULL;
    if (dbPath == NULL || command == NULL ||
        sqlite3_open_v2(dbPath, &db, SQLITE_OPEN_READWRITE, NULL) !=
            SQLITE_OK)
        goto failed;
    /* SQLite busy handlers are local to each database connection. */
    busyTimeout = getenv("LSB_BHIST_DB_BUSY_TIMEOUT");
    if (busyTimeout != NULL &&
        sqlite3_busy_timeout(db, atoi(busyTimeout)) != SQLITE_OK)
        goto failed;
    rc = sqlite3_prepare_v2(db, insertSql, -1, &statement, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(statement, 1, completedAtMs);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(statement, 2, pending->query.userHint,
                               -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(statement, 3, command, -1,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(statement, 4, queueMs);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(statement, 5, executionMs);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(statement, 6, exitStatus);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(statement);
    if (rc == SQLITE_DONE)
        goto done;

failed:
    serviceLog("request_history_write_failed", pending->requestId,
               "error=%s", db != NULL ? sqlite3_errmsg(db) :
               "cannot open database or format command");
done:
    if (statement != NULL)
        sqlite3_finalize(statement);
    if (db != NULL)
        sqlite3_close(db);
    free(command);
}

/*
 * Run the query function in a child and relay output and exit status to a
 * client.
 * @param[in,out] pending: Query request and client connection.
 * @param[in,out] metrics: Output-byte counter shared with the parent.
 * @return: 0 on success, otherwise -1 on disconnect or execution failure.
 */
static int
relayQuery(struct pendingRequest *pending,
           struct handlerMetrics *metrics)
{
    struct pollfd pollFds[3];
    unsigned char buffer[PROTOCOL_DATA_MAX];
    unsigned char exitPayload[8];
    char **childArgv;
    long long completedAtMs;
    long long finishedAt;
    long long queueMs;
    long long startedAt;
    long long cancelDeadline;
    long long stdoutBytes;
    long long stderrBytes;
    pid_t child;
    pid_t waited;
    int stdoutPipe[2];
    int stderrPipe[2];
    int waitOptions;
    int stdoutOpen;
    int stderrOpen;
    int childDone;
    int childStatus;
    int disconnected;
    int setupErrno;
    uint32_t reportedExitCode;
    uint32_t reportedSignal;
    int exitStatus;
    int i;
    int nfds;
    int rc;

    handlerClientFd = pending->fd;
    handlerStopRequested = 0;
    installHandlerSignals();

    if (pipe(stdoutPipe) != 0) {
        setupErrno = errno;
        serviceLog("query_setup_failed", pending->requestId,
                   "peer=%s stage=stdout_pipe error=%s",
                   pending->peer, strerror(setupErrno));
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot create query stdout pipe");
        return -1;
    }
    if (pipe(stderrPipe) != 0) {
        setupErrno = errno;
        serviceLog("query_setup_failed", pending->requestId,
                   "peer=%s stage=stderr_pipe error=%s",
                   pending->peer, strerror(setupErrno));
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot create query stderr pipe");
        return -1;
    }
    (void)fcntl(stdoutPipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(stdoutPipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(stderrPipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(stderrPipe[1], F_SETFD, FD_CLOEXEC);

    /* Do not let the query child flush buffered output inherited from server. */
    (void)fflush(NULL);
    child = fork();
    setupErrno = errno;
    if (child < 0) {
        serviceLog("query_setup_failed", pending->requestId,
                   "peer=%s stage=query_fork error=%s",
                   pending->peer, strerror(setupErrno));
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot start query process");
        return -1;
    }
    if (child == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGCHLD, SIG_DFL);
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        if (dup2(stdoutPipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderrPipe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);
        close(pending->fd);
        handlerClientFd = -1;
        handlerStopRequested = 0;

        if (setenv("LSB_BHIST_INTERNAL_REQUEST_USER",
                   pending->query.userHint, 1) != 0)
            _exit(127);

        childArgv = (char **)calloc((size_t)pending->query.argc + 2,
                                     sizeof(char *));
        if (childArgv == NULL)
            _exit(127);
        childArgv[0] = "bhist-speedup";
        for (i = 0; i < pending->query.argc; i++)
            childArgv[i + 1] = pending->query.argv[i];
        childArgv[pending->query.argc + 1] = NULL;
        rc = runQuery(pending->query.argc + 1, childArgv);
        free(childArgv);
        (void)fflush(NULL);
        _exit(rc);
    }

    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    startedAt = monotonicMillis();
    stdoutBytes = 0;
    stderrBytes = 0;
    stdoutOpen = 1;
    stderrOpen = 1;
    childDone = 0;
    childStatus = 0;
    disconnected = 0;
    serviceLog("query_started", pending->requestId,
               "peer=%s user_hint=%s argc=%d queued_ms=%lld",
               pending->peer, pending->query.userHint, pending->query.argc,
               startedAt - pending->enqueuedAt);

    while (!handlerStopRequested) {
        memset(pollFds, 0, sizeof(pollFds));
        nfds = 0;
        pollFds[nfds].fd = pending->fd;
        pollFds[nfds].events = POLLIN | POLLHUP | POLLERR;
        nfds++;
        if (stdoutOpen) {
            pollFds[nfds].fd = stdoutPipe[0];
            pollFds[nfds].events = POLLIN | POLLHUP | POLLERR;
            nfds++;
        }
        if (stderrOpen) {
            pollFds[nfds].fd = stderrPipe[0];
            pollFds[nfds].events = POLLIN | POLLHUP | POLLERR;
            nfds++;
        }

        rc = poll(pollFds, (nfds_t)nfds, 500);
        if (rc < 0 && errno != EINTR) {
            handlerStopRequested = 1;
            break;
        }
        if (pollFds[0].revents != 0) {
            unsigned char unexpected;
            ssize_t peeked;

            peeked = recv(pending->fd, &unexpected, 1,
                          MSG_PEEK | MSG_DONTWAIT);
            if (peeked == 0 ||
                (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ||
                peeked > 0) {
                disconnected = 1;
                handlerStopRequested = 1;
            }
        }

        for (i = 1; i < nfds && !handlerStopRequested; i++) {
            ssize_t bytesRead;
            uint8_t frameType;

            if (pollFds[i].revents == 0)
                continue;
            bytesRead = read(pollFds[i].fd, buffer, sizeof(buffer));
            if (bytesRead > 0) {
                frameType = pollFds[i].fd == stdoutPipe[0] ?
                    FRAME_STDOUT : FRAME_STDERR;
                rc = sendFrame(pending->fd, frameType, buffer,
                                    (uint32_t)bytesRead);
                if (rc != 0) {
                    disconnected = 1;
                    handlerStopRequested = 1;
                    break;
                }
                if (frameType == FRAME_STDOUT)
                    stdoutBytes += bytesRead;
                else
                    stderrBytes += bytesRead;
                __atomic_store_n(
                    &metrics->outputBytes,
                    (unsigned long long)(stdoutBytes + stderrBytes),
                    __ATOMIC_RELAXED);
            } else if (bytesRead == 0) {
                if (pollFds[i].fd == stdoutPipe[0]) {
                    close(stdoutPipe[0]);
                    stdoutOpen = 0;
                } else {
                    close(stderrPipe[0]);
                    stderrOpen = 0;
                }
            } else if (errno != EINTR) {
                handlerStopRequested = 1;
                break;
            }
        }

        if (!childDone) {
            /*
             * pipe EOF can become visible just before the exiting child is
             * waitable.  Once both streams are drained, block for that final
             * transition instead of entering another 500 ms socket poll.
             * Keep cancellation nonblocking so the process-group cleanup
             * below can still terminate a running query.
             */
            waitOptions = !handlerStopRequested &&
                !stdoutOpen && !stderrOpen ? 0 : WNOHANG;
            do {
                waited = waitpid(child, &childStatus, waitOptions);
            } while (waited < 0 && errno == EINTR &&
                     !handlerStopRequested);
            if (waited == child)
                childDone = 1;
            else if (waited < 0 && errno != EINTR) {
                handlerStopRequested = 1;
                break;
            }
        }
        if (childDone && !stdoutOpen && !stderrOpen)
            break;
    }

    if (handlerStopRequested && !childDone) {
        /* Signal the negative PGID to clean up query and every bhist descendant. */
        kill(-getpgrp(), SIGTERM);
        cancelDeadline = nowMillis() + HANDLER_CANCEL_GRACE_MS;
        while (nowMillis() < cancelDeadline) {
            waited = waitpid(child, &childStatus, WNOHANG);
            if (waited == child) {
                childDone = 1;
                break;
            }
            usleep(100000);
        }
        if (!childDone)
            kill(-getpgrp(), SIGKILL);
    }
    if (!childDone) {
        do {
            waited = waitpid(child, &childStatus, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == child)
            childDone = 1;
        else
            handlerStopRequested = 1;
    }
    if (stdoutOpen)
        close(stdoutPipe[0]);
    if (stderrOpen)
        close(stderrPipe[0]);

    if (handlerStopRequested) {
        finishedAt = monotonicMillis();
        exitStatus = childDone && WIFEXITED(childStatus) ?
            WEXITSTATUS(childStatus) :
            childDone && WIFSIGNALED(childStatus) ?
            128 + WTERMSIG(childStatus) : 255;
        serviceLog(disconnected ? "query_disconnected" : "query_cancelled",
                   pending->requestId,
                   "peer=%s elapsed_ms=%lld stdout_bytes=%lld "
                   "stderr_bytes=%lld",
                   pending->peer, finishedAt - startedAt,
                   stdoutBytes, stderrBytes);
        recordQueryHistory(pending, nowMillis(),
                           startedAt - pending->enqueuedAt,
                           finishedAt - startedAt, exitStatus);
        return -1;
    }

    if (WIFEXITED(childStatus)) {
        reportedExitCode = (uint32_t)WEXITSTATUS(childStatus);
        reportedSignal = 0;
    } else if (WIFSIGNALED(childStatus)) {
        reportedExitCode = 0;
        reportedSignal = (uint32_t)WTERMSIG(childStatus);
    } else {
        reportedExitCode = 255;
        reportedSignal = 0;
    }
    writeU32(exitPayload, reportedExitCode);
    writeU32(exitPayload + 4, reportedSignal);
    finishedAt = monotonicMillis();
    completedAtMs = nowMillis();
    queueMs = startedAt - pending->enqueuedAt;
    exitStatus = reportedSignal != 0 ?
        128 + (int)reportedSignal : (int)reportedExitCode;
    rc = sendFrame(pending->fd, FRAME_EXIT, exitPayload,
                        sizeof(exitPayload));
    recordQueryHistory(pending, completedAtMs, queueMs,
                       finishedAt - startedAt, exitStatus);
    serviceLog("query_finished", pending->requestId,
               "peer=%s elapsed_ms=%lld exit_code=%u signal=%u "
               "stdout_bytes=%lld stderr_bytes=%lld",
               pending->peer, finishedAt - startedAt,
               reportedExitCode, reportedSignal,
               stdoutBytes, stderrBytes);
    return rc;
}

/*
 * Start a handler for a pending request and add it to the active list.
 * @param[in,out] pending: Request to start, consumed on success.
 * @param[in] listenerFd: Server listener descriptor.
 * @param[in] queue: Remaining queue whose inherited descriptors are closed.
 * @param[in,out] activeHead: Head of the active-handler list.
 * @param[in,out] activeCount: Number of active handlers.
 * @return: 0 on success, otherwise -1.
 */
static int
startRequest(struct pendingRequest *pending,
             int listenerFd, const struct pendingRequest *queue,
             struct activeHandler **activeHead, int *activeCount)
{
    struct activeHandler *active;
    struct handlerMetrics *handlerMetrics;
    int forkErrno;
    pid_t pid;

    active = (struct activeHandler *)calloc(1, sizeof(*active));
    if (active == NULL) {
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot track query handler");
        freePendingRequest(pending);
        return -1;
    }
    /* mmap shares only the output counter, never query data or sockets. */
    handlerMetrics = mmap(NULL, sizeof(*handlerMetrics),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (handlerMetrics == MAP_FAILED) {
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot allocate query metrics");
        free(active);
        freePendingRequest(pending);
        return -1;
    }
    __atomic_store_n(&handlerMetrics->outputBytes, 0, __ATOMIC_RELAXED);

    pid = fork();
    forkErrno = errno;
    if (pid < 0) {
        sendErrorFrame(pending->fd, SERVICE_ERROR_INTERNAL,
                       "cannot start query handler");
        serviceLog("handler_fork_failed", pending->requestId,
                   "peer=%s error=%s", pending->peer,
                   strerror(forkErrno));
        munmap(handlerMetrics, sizeof(*handlerMetrics));
        free(active);
        freePendingRequest(pending);
        return -1;
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0)
            _exit(1);
        close(listenerFd);
        closeQueuedFds(queue, pending->fd);
        (void)relayQuery(pending, handlerMetrics);
        freePendingRequest(pending);
        _exit(0);
    }
    (void)setpgid(pid, pid);

    active->pid = pid;
    active->requestId = pending->requestId;
    active->requestClass = pending->requestClass;
    active->metrics = handlerMetrics;
    active->next = *activeHead;
    *activeHead = active;
    *activeCount += 1;
    serviceLog("handler_started", pending->requestId,
               "peer=%s handler_pid=%ld queued_ms=%lld running=%d",
               pending->peer, (long)pid,
               monotonicMillis() - pending->enqueuedAt, *activeCount);

    /* The parent must close its copy so handler close produces peer-visible EOF. */
    close(pending->fd);
    pending->fd = -1;
    freePendingRequest(pending);
    return 0;
}

/*
 * Insert a request into the bounded wait queue.
 * @param[in,out] head: Queue head.
 * @param[in,out] tail: Queue tail.
 * @param[in] request: Request to insert; job IDs go to the head, scans to tail.
 * @return: None.
 */
static void
appendQueue(struct pendingRequest **head, struct pendingRequest **tail,
            struct pendingRequest *request)
{
    request->next = NULL;
    if (*head == NULL) {
        *head = request;
        *tail = request;
        return;
    }
    if (request->requestClass == REQUEST_CLASS_JOB_ID) {
        request->next = *head;
        *head = request;
        return;
    }
    (*tail)->next = request;
    *tail = request;
}

/*
 * Remove and return the request at the queue head.
 * @param[in,out] head: Queue head.
 * @param[in,out] tail: Queue tail, cleared when the queue becomes empty.
 * @return: Detached request, or NULL for an empty queue.
 */
static struct pendingRequest *
popQueue(struct pendingRequest **head, struct pendingRequest **tail)
{
    struct pendingRequest *request;

    request = *head;
    if (request == NULL)
        return NULL;
    *head = request->next;
    if (*head == NULL)
        *tail = NULL;
    request->next = NULL;
    return request;
}

/*
 * Count active scan handlers.
 * @param[in] activeHead: Head of the active-handler list.
 * @return: Number of active scan handlers.
 */
static int
countActiveScans(const struct activeHandler *activeHead)
{
    const struct activeHandler *active;
    int count;

    count = 0;
    for (active = activeHead; active != NULL; active = active->next) {
        if (active->requestClass != REQUEST_CLASS_JOB_ID)
            count++;
    }
    return count;
}

/*
 * Unlink and release a specific disconnected request from the wait queue.
 * @param[in,out] head: Queue head.
 * @param[in,out] tail: Queue tail.
 * @param[in] target: Request to remove.
 * @param[in,out] queuedCount: Number of queued requests.
 * @return: None.
 */
static void
removeQueuedRequest(struct pendingRequest **head,
                    struct pendingRequest **tail,
                    struct pendingRequest *target, int *queuedCount)
{
    struct pendingRequest *previous;
    struct pendingRequest *cursor;

    previous = NULL;
    cursor = *head;
    while (cursor != NULL && cursor != target) {
        previous = cursor;
        cursor = cursor->next;
    }
    if (cursor == NULL)
        return;
    if (previous == NULL)
        *head = cursor->next;
    else
        previous->next = cursor->next;
    if (*tail == cursor)
        *tail = previous;
    *queuedCount -= 1;
    serviceLog("queue_disconnected", cursor->requestId,
               "peer=%s queued_ms=%lld", cursor->peer,
               monotonicMillis() - cursor->enqueuedAt);
    freePendingRequest(cursor);
}

/*
 * Start eligible queued work while respecting total and scan limits.
 * @param[in] config: Service concurrency limits.
 * @param[in,out] queueHead: Queue head.
 * @param[in,out] queueTail: Queue tail.
 * @param[in,out] queuedCount: Number of queued requests.
 * @param[in] listenerFd: Server listener descriptor.
 * @param[in,out] activeHead: Active-handler list.
 * @param[in,out] activeCount: Number of running handlers.
 * @return: None.
 */
static void
dispatchQueue(const struct serviceConfig *config,
              struct pendingRequest **queueHead,
              struct pendingRequest **queueTail, int *queuedCount,
              int listenerFd, struct activeHandler **activeHead,
              int *activeCount)
{
    struct pendingRequest *pending;

    while (*activeCount < config->maxRunning && *queueHead != NULL) {
        pending = *queueHead;
        if (pending->requestClass != REQUEST_CLASS_JOB_ID &&
            countActiveScans(*activeHead) >= config->maxScanRunning)
            break;
        pending = popQueue(queueHead, queueTail);
        *queuedCount -= 1;
        if (startRequest(pending, listenerFd, *queueHead,
                         activeHead, activeCount) != 0)
            continue;
    }
}

/*
 * Transfer newly reported handler output bytes into server metrics.
 * @param[in,out] active: Active handler with shared output accounting.
 * @return: None.
 */
static void
collectHandlerOutput(struct activeHandler *active)
{
    uint64_t currentBytes;

    currentBytes = (uint64_t)__atomic_load_n(
        &active->metrics->outputBytes, __ATOMIC_RELAXED);
    if (currentBytes > active->collectedOutputBytes) {
        metricsRecordOutput(
            &serviceMetrics,
            currentBytes - active->collectedOutputBytes);
    }
    active->collectedOutputBytes = currentBytes;
}

/*
 * Collect output-byte deltas from every active handler.
 * @param[in,out] activeHead: Head of the active-handler list.
 * @return: None.
 */
static void
collectActiveOutput(struct activeHandler *activeHead)
{
    struct activeHandler *active;

    for (active = activeHead; active != NULL; active = active->next)
        collectHandlerOutput(active);
}

/*
 * Start fixed-period in-memory performance monitoring.
 * @param[in] periodSeconds: Sample period in seconds.
 * @param[in] activeHead: Current active-handler list.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
startPerformanceMonitor(unsigned periodSeconds,
                        struct activeHandler *activeHead,
                        char *error, size_t errorSize)
{
    long long startedMs;
    time_t started;

    startedMs = monotonicMillis();
    started = time(NULL);
    if (startedMs <= 0 || started < 0) {
        snprintf(error, errorSize, "cannot read the system clock");
        return -1;
    }
    collectActiveOutput(activeHead);
    memset(&serviceMonitor, 0, sizeof(serviceMonitor));
    serviceMonitor.enabled = 1;
    serviceMonitor.periodSeconds = periodSeconds;
    serviceMonitor.nextSampleMs = startedMs +
        (long long)periodSeconds * 1000LL;
    serviceMonitor.monitorStartTime = (uint64_t)started;
    metricsStart(&serviceMetrics);
    serviceLog("perfmon_started", 0, "period_seconds=%u", periodSeconds);
    return 0;
}

/*
 * Disable perfmon and discard its schedule and counters.
 * @return: None.
 */
static void
stopPerformanceMonitor(void)
{
    if (serviceMonitor.enabled)
        serviceLog("perfmon_stopped", 0, "period_seconds=%u",
                   serviceMonitor.periodSeconds);
    memset(&serviceMonitor, 0, sizeof(serviceMonitor));
    metricsStop(&serviceMetrics);
}

/*
 * Complete a performance period at its boundary and schedule the next one.
 * @param[in] activeHead: Current active-handler list.
 * @return: None.
 */
static void
samplePerformanceIfDue(struct activeHandler *activeHead)
{
    long long nowMs;
    long long periodMs;
    uint64_t elapsedPeriods;

    if (!serviceMonitor.enabled)
        return;
    nowMs = monotonicMillis();
    if (nowMs < serviceMonitor.nextSampleMs)
        return;

    collectActiveOutput(activeHead);
    metricsCompletePeriod(&serviceMetrics);

    periodMs = (long long)serviceMonitor.periodSeconds * 1000LL;
    elapsedPeriods = 1U;
    if (nowMs > serviceMonitor.nextSampleMs)
        elapsedPeriods +=
            (uint64_t)(nowMs - serviceMonitor.nextSampleMs) /
            (uint64_t)periodMs;
    serviceMonitor.elapsedPeriods += elapsedPeriods;
    serviceMonitor.lastSampleEndTime =
        serviceMonitor.monitorStartTime +
        serviceMonitor.elapsedPeriods * serviceMonitor.periodSeconds;
    serviceMonitor.nextSampleMs +=
        (long long)elapsedPeriods * periodMs;
}

/*
 * Calculate the next server-loop timeout without crossing a sample boundary.
 * @return: Poll timeout in milliseconds between zero and 1000.
 */
static int
performancePollTimeout(void)
{
    long long remainingMs;

    if (!serviceMonitor.enabled)
        return 1000;
    remainingMs = serviceMonitor.nextSampleMs - monotonicMillis();
    if (remainingMs <= 0)
        return 0;
    if (remainingMs > 1000)
        return 1000;
    return (int)remainingMs;
}

/*
 * Reap completed handlers, collect their final metrics, and clean descendants.
 * @param[in,out] activeHead: Active-handler list.
 * @param[in,out] activeCount: Number of running handlers.
 * @return: None.
 */
static void
reapHandlers(struct activeHandler **activeHead, int *activeCount)
{
    struct activeHandler *previous;
    struct activeHandler *cursor;
    pid_t pid;
    int status;

    parentChildChanged = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        previous = NULL;
        cursor = *activeHead;
        while (cursor != NULL && cursor->pid != pid) {
            previous = cursor;
            cursor = cursor->next;
        }
        if (cursor == NULL)
            continue;
        collectHandlerOutput(cursor);
        /* A crashed handler must not leave its query or bhist workers alive. */
        (void)kill(-pid, SIGKILL);
        if (previous == NULL)
            *activeHead = cursor->next;
        else
            previous->next = cursor->next;
        *activeCount -= 1;
        serviceLog("handler_reaped", cursor->requestId,
                   "handler_pid=%ld status=%d running=%d",
                   (long)pid, status, *activeCount);
        munmap(cursor->metrics, sizeof(*cursor->metrics));
        free(cursor);
    }
}

/*
 * Receive, validate, and dispatch one client request.
 * @param[in] config: Service runtime configuration.
 * @param[in] listenerFd: Server listener descriptor.
 * @param[in,out] queueHead: Wait-queue head.
 * @param[in,out] queueTail: Wait-queue tail.
 * @param[in,out] queuedCount: Number of queued requests.
 * @param[in,out] activeHead: Active-handler list head.
 * @param[in,out] activeCount: Number of active handlers.
 * @param[in,out] nextRequestId: Next query request identifier.
 * @return: 0 after handling the request, otherwise -1 on accept or send failure.
 */
static int
receiveConnection(const struct serviceConfig *config, int listenerFd,
                  struct pendingRequest **queueHead,
                  struct pendingRequest **queueTail, int *queuedCount,
                  struct activeHandler **activeHead, int *activeCount,
                  unsigned long long *nextRequestId)
{
    struct pendingRequest *pending;
    unsigned char *payload;
    const char *rejectReason;
    char peer[SERVICE_PEER_MAX];
    uint32_t payloadLength;
    uint8_t type;
    int acceptErrno;
    int userHintRc;
    int decodeRc;
    int fd;
    int frameErrno;
    int flags;
    int rc;
    int timeoutRc;
    int timeoutErrno;

    fd = accept(listenerFd, NULL, NULL);
    acceptErrno = errno;
    if (fd < 0) {
        errno = acceptErrno;
        return acceptErrno == EINTR ? 0 : -1;
    }
    describePeer(fd, peer, sizeof(peer));
    configureConnectedSocket(fd);
    flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    timeoutRc = setReceiveTimeout(fd, config->requestTimeoutSeconds);
    timeoutErrno = errno;
    serviceLog("connection_accepted", 0,
               "fd=%d peer=%s running=%d queued=%d",
               fd, peer, *activeCount, *queuedCount);
    if (timeoutRc != 0)
        serviceLog("socket_setup_failed", 0,
                   "fd=%d peer=%s operation=recv_timeout error=%s",
                   fd, peer, strerror(timeoutErrno));

    payload = NULL;
    rc = receiveFrame(fd, &type, &payload, &payloadLength,
                        PROTOCOL_REQUEST_MAX);
    frameErrno = errno;
    clearRecvTimeout(fd);
    if (rc != 0) {
        serviceLog("request_frame_failed", 0,
                   "fd=%d peer=%s rc=%d error=%s",
                   fd, peer, rc,
                   rc < 0 ? strerror(frameErrno) : "connection_closed");
        if (rc < 0)
            sendErrorFrame(fd, SERVICE_ERROR_PROTOCOL,
                           "invalid or incomplete request frame");
        free(payload);
        close(fd);
        return 0;
    }
    serviceLog("request_frame_received", 0,
               "fd=%d peer=%s type=%u payload_bytes=%u",
               fd, peer, (unsigned int)type, payloadLength);
    samplePerformanceIfDue(*activeHead);
    if (type == FRAME_STATUS && payloadLength == 0) {
        rc = sendStatusFrame(fd, *activeCount, *queuedCount, config);
        serviceLog("status_served", 0,
                   "fd=%d peer=%s running=%d queued=%d send_rc=%d",
                   fd, peer, *activeCount, *queuedCount,
                   rc);
        free(payload);
        close(fd);
        return 0;
    }
    if (type == FRAME_PERFMON_START && payloadLength == 4) {
        unsigned periodSeconds;
        char monitorError[256];

        periodSeconds = readU32(payload);
        if (periodSeconds < PERFMON_MIN_PERIOD_SECONDS ||
            periodSeconds > PERFMON_MAX_PERIOD_SECONDS) {
            rc = sendErrorFrame(fd, SERVICE_ERROR_PROTOCOL,
                                "invalid performance sample period");
        } else if (startPerformanceMonitor(
                       periodSeconds, *activeHead,
                       monitorError, sizeof(monitorError)) != 0) {
            rc = sendErrorFrame(fd, SERVICE_ERROR_INTERNAL, monitorError);
        } else {
            rc = sendPerfmonResult(fd);
        }
        free(payload);
        close(fd);
        return rc == 0 ? 0 : -1;
    }
    if (type == FRAME_PERFMON_STOP && payloadLength == 0) {
        collectActiveOutput(*activeHead);
        stopPerformanceMonitor();
        rc = sendPerfmonResult(fd);
        free(payload);
        close(fd);
        return rc == 0 ? 0 : -1;
    }
    if (type == FRAME_PERFORMANCE_VIEW && payloadLength == 0) {
        rc = sendPerformanceFrame(fd);
        serviceLog("performance_view_served", 0,
                   "fd=%d peer=%s monitor_started=%d send_rc=%d",
                   fd, peer, serviceMonitor.enabled, rc);
        free(payload);
        close(fd);
        return 0;
    }
    if (type != FRAME_QUERY) {
        serviceLog("request_rejected", 0,
                   "fd=%d peer=%s reason=unsupported_frame type=%u "
                   "payload_bytes=%u",
                   fd, peer, (unsigned int)type, payloadLength);
        sendErrorFrame(fd, SERVICE_ERROR_PROTOCOL,
                       "unsupported request frame type");
        free(payload);
        close(fd);
        return 0;
    }

    pending = (struct pendingRequest *)calloc(1, sizeof(*pending));
    if (pending == NULL) {
        serviceLog("request_rejected", 0,
                   "fd=%d peer=%s reason=out_of_memory", fd, peer);
        sendErrorFrame(fd, SERVICE_ERROR_INTERNAL, "out of memory");
        free(payload);
        close(fd);
        return 0;
    }
    pending->fd = fd;
    pending->requestId = (*nextRequestId)++;
    pending->enqueuedAt = monotonicMillis();
    snprintf(pending->peer, sizeof(pending->peer), "%s", peer);
    decodeRc = decodeQueryRequest(payload, payloadLength,
                                         &pending->query);
    userHintRc = decodeRc == 0 ?
        validateUserHint(pending->query.userHint) : -1;
    /*
     * userHint is a client-supplied default filter, not an authenticated
     * identity. An explicit -u remains authoritative in the query backend.
     */
    if (decodeRc != 0 || userHintRc != 0) {
        rejectReason = decodeRc != 0 ? "decode" :
            "user_hint";
        serviceLog("request_rejected", pending->requestId,
                   "fd=%d peer=%s reason=%s payload_bytes=%u",
                   fd, pending->peer, rejectReason, payloadLength);
        sendErrorFrame(fd, SERVICE_ERROR_PROTOCOL,
                       "invalid query request");
        free(payload);
        freePendingRequest(pending);
        return 0;
    }
    /*
     * Decoding copied userHint and argv into pending->query. The original frame
     * can now be freed; freePendingRequest() owns all decoded query storage.
     */
    free(payload);
    pending->requestClass = classifyRequestArgs(
        pending->query.argc, pending->query.argv);
    metricsRecordRequest(&serviceMetrics,
                               pending->requestClass);
    serviceLog("request_accepted", pending->requestId,
               "fd=%d peer=%s user_hint=%s argc=%d payload_bytes=%u "
               "running=%d queued=%d",
               fd,
               pending->peer, pending->query.userHint, pending->query.argc,
               payloadLength, *activeCount, *queuedCount);

    if (pending->requestClass == REQUEST_CLASS_JOB_ID &&
        *activeCount < config->maxRunning) {
        return startRequest(pending, listenerFd, *queueHead,
                            activeHead, activeCount);
    }
    if (pending->requestClass != REQUEST_CLASS_JOB_ID &&
        *queueHead == NULL &&
        *activeCount < config->maxRunning &&
        countActiveScans(*activeHead) <
            config->maxScanRunning) {
        return startRequest(pending, listenerFd, *queueHead,
                            activeHead, activeCount);
    }
    if (*queuedCount >= config->maxQueued) {
        sendErrorFrame(fd, SERVICE_ERROR_BUSY, "service queue is full");
        serviceLog("queue_full", pending->requestId,
                   "peer=%s running=%d queued=%d",
                   pending->peer, *activeCount, *queuedCount);
        freePendingRequest(pending);
        return 0;
    }

    appendQueue(queueHead, queueTail, pending);
    *queuedCount += 1;
    serviceLog("request_queued", pending->requestId,
               "peer=%s class=%s queued=%d running=%d",
               pending->peer,
               pending->requestClass == REQUEST_CLASS_JOB_ID ?
                   "job_id" : "scan",
               *queuedCount, *activeCount);
    return 0;
}

/*
 * Poll queued client sockets and remove requests whose peers disconnected.
 * @param[in] listenerFd: Server listener included to wake for new work.
 * @param[in,out] queueHead: Queue head.
 * @param[in,out] queueTail: Queue tail.
 * @param[in,out] queuedCount: Number of queued requests.
 * @return: None.
 */
static void
watchQueuedClients(int listenerFd, struct pendingRequest **queueHead,
                   struct pendingRequest **queueTail, int *queuedCount)
{
    struct pendingRequest **requests;
    struct pendingRequest *cursor;
    struct pollfd *pollFds;
    unsigned char unexpected;
    ssize_t peeked;
    int count;
    int i;
    int rc;

    count = *queuedCount + 1;
    pollFds = (struct pollfd *)calloc((size_t)count, sizeof(*pollFds));
    requests = (struct pendingRequest **)calloc(
        (size_t)*queuedCount, sizeof(*requests));
    if (pollFds == NULL || requests == NULL) {
        free(pollFds);
        free(requests);
        poll(NULL, 0, 100);
        return;
    }
    pollFds[0].fd = listenerFd;
    pollFds[0].events = POLLIN | POLLERR;
    cursor = *queueHead;
    i = 0;
    while (cursor != NULL && i < *queuedCount) {
        requests[i] = cursor;
        pollFds[i + 1].fd = cursor->fd;
        pollFds[i + 1].events = POLLIN | POLLHUP | POLLERR;
        cursor = cursor->next;
        i++;
    }

    rc = poll(pollFds, (nfds_t)count, 1000);
    if (rc > 0) {
        for (i = 0; i < count - 1; i++) {
            if (pollFds[i + 1].revents == 0)
                continue;
            peeked = recv(pollFds[i + 1].fd, &unexpected, 1,
                          MSG_PEEK | MSG_DONTWAIT);
            if (peeked == 0 || peeked > 0 ||
                (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                if (peeked > 0)
                    sendErrorFrame(pollFds[i + 1].fd,
                                   SERVICE_ERROR_PROTOCOL,
                                   "unexpected data while queued");
                removeQueuedRequest(queueHead, queueTail, requests[i],
                                    queuedCount);
            }
        }
    }
    if (rc > 0 && pollFds[0].revents != 0) {
        /* The caller handles accept after this short readiness wait. */
    }
    free(requests);
    free(pollFds);
}

/*
 * Stop accepting requests, reject the queue, and clean up handler groups.
 * @param[in] listenerFd: Server listener descriptor.
 * @param[in,out] queueHead: Wait-queue head.
 * @param[in,out] queueTail: Wait-queue tail.
 * @param[in,out] queuedCount: Number of queued requests.
 * @param[in,out] activeHead: Active-handler list head.
 * @param[in,out] activeCount: Number of active handlers.
 * @return: None.
 */
static void
shutdownService(int listenerFd, struct pendingRequest **queueHead,
                struct pendingRequest **queueTail, int *queuedCount,
                struct activeHandler **activeHead, int *activeCount)
{
    struct pendingRequest *pending;
    struct activeHandler *active;
    long long deadline;

    if (listenerFd >= 0)
        close(listenerFd);
    while ((pending = popQueue(queueHead, queueTail)) != NULL) {
        sendErrorFrame(pending->fd, SERVICE_ERROR_SHUTDOWN,
                       "service is shutting down");
        *queuedCount -= 1;
        freePendingRequest(pending);
    }

    for (active = *activeHead; active != NULL; active = active->next)
        kill(-active->pid, SIGTERM);
    deadline = nowMillis() + SERVICE_SHUTDOWN_GRACE_MS;
    while (*activeCount > 0 && nowMillis() < deadline) {
        reapHandlers(activeHead, activeCount);
        if (*activeCount > 0)
            usleep(100000);
    }
    for (active = *activeHead; active != NULL; active = active->next)
        kill(-active->pid, SIGKILL);
    while (*activeCount > 0) {
        reapHandlers(activeHead, activeCount);
        if (*activeCount > 0)
            usleep(100000);
    }
    *queueTail = NULL;
}

/*
 * Run the single-threaded listener loop and dispatch query handlers.
 * @param[in] sharedConfig: Loaded server configuration.
 * @return: 0 on normal shutdown, otherwise 1 on listener failure.
 */
static int
runService(const struct config *sharedConfig)
{
    struct serviceConfig config;
    struct pendingRequest *queueHead;
    struct pendingRequest *queueTail;
    struct activeHandler *activeHead;
    unsigned long long nextRequestId;
    int activeCount;
    int queuedCount;
    int listenerFd;

    memset(&config, 0, sizeof(config));
    config.endpoint = sharedConfig->endpoint;
    config.maxRunning = sharedConfig->maxConcurrentQueries;
    config.maxScanRunning = sharedConfig->maxConcurrentScanQueries;
    config.maxQueued = sharedConfig->maxQueuedQueries;
    config.requestTimeoutSeconds =
        sharedConfig->receiveTimeoutSeconds;

    listenerFd = listenEndpoint(
        config.endpoint,
        config.maxRunning + config.maxQueued + 16);
    if (listenerFd < 0) {
        fprintf(stderr, "bhist-speedup-server: cannot listen on %s: %s\n",
                config.endpoint, strerror(errno));
        return 1;
    }
    installParentSignals();
    queueHead = NULL;
    queueTail = NULL;
    activeHead = NULL;
    activeCount = 0;
    queuedCount = 0;
    nextRequestId = 1;
    metricsInit(&serviceMetrics);
    memset(&serviceMonitor, 0, sizeof(serviceMonitor));
    serviceLog("service_ready", 0,
               "endpoint=%s max_handlers=%d "
               "max_scan_running=%d max_queued=%d",
               config.endpoint, config.maxRunning,
               config.maxScanRunning, config.maxQueued);
    /*
     * Sampling, reaping, priority dispatch, and accept mutate global state only
     * in this loop. Handler output uses a lock-free atomic counter, so queue and
     * metric state need no additional locks.
     */
    while (!parentStopRequested) {
        struct pollfd listenerPoll;
        int receiveErrno;
        int receiveRc;
        int pollErrno;
        int rc;

        samplePerformanceIfDue(activeHead);
        if (parentChildChanged)
            reapHandlers(&activeHead, &activeCount);
        dispatchQueue(&config, &queueHead, &queueTail, &queuedCount,
                      listenerFd, &activeHead, &activeCount);

        memset(&listenerPoll, 0, sizeof(listenerPoll));
        listenerPoll.fd = listenerFd;
        listenerPoll.events = POLLIN | POLLERR;
        rc = poll(&listenerPoll, 1,
                  queuedCount > 0 ? 0 : performancePollTimeout());
        pollErrno = errno;
        if (rc > 0 && listenerPoll.revents != 0) {
            serviceLog("listener_ready", 0,
                       "revents=%d running=%d queued=%d",
                       listenerPoll.revents,
                       activeCount, queuedCount);
            receiveRc = receiveConnection(
                &config, listenerFd, &queueHead, &queueTail,
                &queuedCount, &activeHead, &activeCount,
                &nextRequestId);
            receiveErrno = errno;
            if (receiveRc != 0 && receiveErrno != EINTR)
                serviceLog("accept_failed", 0, "error=%s",
                           strerror(receiveErrno));
        } else if (rc < 0 && pollErrno != EINTR) {
            serviceLog("listen_poll_failed", 0, "error=%s",
                       strerror(pollErrno));
        }
        if (queuedCount > 0) {
            watchQueuedClients(listenerFd, &queueHead, &queueTail,
                               &queuedCount);
        }
    }

    serviceLog("service_stopping", 0,
               "running=%d queued=%d", activeCount, queuedCount);
    stopPerformanceMonitor();
    shutdownService(listenerFd, &queueHead, &queueTail, &queuedCount,
                    &activeHead, &activeCount);
    serviceLog("service_stopped", 0, "running=0 queued=0");
    return 0;
}

/*
 * Store an integer in an environment variable inherited by query children.
 * @param[in] name: Environment variable name.
 * @param[in] value: Integer value.
 * @return: 0 on success, otherwise -1.
 */
static int
setIntegerEnvironment(const char *name, long long value)
{
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%lld", value);
    return setenv(name, buffer, 1);
}

/*
 * Configure database, bhist, and tuning variables for query children.
 * @param[in] config: Server configuration.
 * @return: 0 on success, otherwise -1.
 */
static int
configureQueryEnvironment(const struct config *config)
{
    return setenv("LSB_BHIST_INTERNAL_DB_PATH",
                  config->dbPath, 1) == 0 &&
        setenv("LSB_BHIST_INTERNAL_BHIST_ORIGINAL_PATH",
               config->bhistOriginalPath, 1) == 0 &&
        setIntegerEnvironment("LSB_BHIST_MAX_FORMATTERS_PER_QUERY",
                              config->formattersPerQuery) == 0 &&
        setIntegerEnvironment("LSB_BHIST_JOBS_PER_BATCH",
                                config->jobsPerBatch) == 0 &&
        setIntegerEnvironment("LSB_BHIST_DB_BUSY_TIMEOUT",
                                config->dbBusyTimeoutMs) == 0 &&
        setIntegerEnvironment("LSB_BHIST_QUERY_DB_CACHE_SIZE",
                                config->queryDbCacheSizeMib) == 0 &&
        setIntegerEnvironment("LSB_BHIST_QUERY_DB_MMAP_SIZE",
                                config->queryDbMmapSizeMib) == 0 ?
        0 : -1;
}

/*
 * Initialize logging and the query environment, then run the server daemon.
 * @param[in] opaque: Pointer to daemonContext.
 * @return: Service-loop exit status.
 */
static int
runDaemon(void *opaque)
{
    struct daemonContext *context;

    context = (struct daemonContext *)opaque;
    ls_openlog("bhist-speedup-server", context->config.logDir, 0,
               context->config.logMask);
    parentStopRequested = 0;
    parentChildChanged = 0;
    installParentSignals();
    if (configureQueryEnvironment(&context->config) != 0) {
        serviceLog("query_environment_failed", 0, "error=%s",
                   strerror(errno));
        return 1;
    }
    serviceLog("database_validation_deferred", 0, "db=%s",
               context->config.dbPath);
    return runService(&context->config);
}

/* Server management command implementation. */

/*
 * Print the public bhist-speedup-server management command syntax.
 * @param[out] stream: Destination stream.
 * @return: None.
 */
static void
usage(FILE *stream)
{
    fputs("Usage: bhist-speedup-server {start|stop|status|view}\n"
          "       bhist-speedup-server perfmon {seconds|stop}\n",
          stream);
}

/*
 * Build the server PID-file path beside the configured database.
 * @param[in] config: Server configuration.
 * @param[out] path: Buffer receiving the PID-file path.
 * @param[in] pathSize: Size of path.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
buildPidPath(const struct config *config, char *path,
                size_t pathSize, char *error, size_t errorSize)
{
    char directory[CONFIG_PATH_MAX];

    if (parentDirectory(config->dbPath, directory,
                             sizeof(directory), error, errorSize) != 0)
        return -1;
    if (snprintf(path, pathSize, "%s/bhist-speedup-server.pid", directory) >=
        (int)pathSize) {
        snprintf(error, errorSize, "server pid path is too long");
        return -1;
    }
    return 0;
}

/*
 * Validate and create directories needed by the server.
 * @param[in] config: Server configuration.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 when directories are usable, otherwise -1.
 */
static int
prepareDirectories(const struct config *config,
                            char *error, size_t errorSize)
{
    char databaseDirectory[CONFIG_PATH_MAX];
    struct stat st;

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
 * Start the server daemon.
 * @param[in] context: Daemon startup context.
 * @param[in] pidPath: PID file path.
 * @param[in] logPath: Daemon log path.
 * @return: 0 when started or already running, otherwise 1.
 */
static int
startServer(struct daemonContext *context, const char *pidPath,
             const char *logPath)
{
    char error[512];
    pid_t pid;
    int result;

    if (prepareDirectories(&context->config,
                                    error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return 1;
    }
    result = daemonStart(pidPath, logPath, runDaemon,
                              context, &pid, error, sizeof(error));
    if (result < 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return 1;
    }
    if (result > 0) {
        printf("bhist-speedup-server: already running pid=%ld\n", (long)pid);
        return 0;
    }
    printf("bhist-speedup-server: started pid=%ld endpoint=%s log=%s\n",
           (long)pid, context->config.endpoint, logPath);
    return 0;
}

/*
 * Stop the server daemon.
 * @param[in] pidPath: PID file path.
 * @return: 0 when stopped or already stopped, otherwise 1.
 */
static int
stopServer(const char *pidPath)
{
    char error[512];
    pid_t pid;
    int wasRunning;

    if (daemonStop(pidPath, 10000, &pid, &wasRunning,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return 1;
    }
    if (wasRunning)
        printf("bhist-speedup-server: stopped pid=%ld\n", (long)pid);
    else
        puts("bhist-speedup-server: not running");
    return 0;
}

/*
 * Determine service state from its PID lock and a control request.
 * @param[in] config: Server configuration.
 * @param[in] pidPath: PID file path.
 * @param[in] printStatus: Whether to print status fields.
 * @param[out] pidOut: Optional running PID.
 * @return: 0 when stopped, 1 while starting, 2 when running, otherwise -1.
 */
static int
serverStatus(const struct config *config, const char *pidPath,
              int printStatus, pid_t *pidOut)
{
    struct controlStatus control;
    char error[512];
    pid_t pid;
    int result;

    result = pidFileStatus(pidPath, &pid, error, sizeof(error));
    if (result < 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return -1;
    }
    if (result == 0) {
        (void)pidFileRemoveStale(pidPath, error, sizeof(error));
        if (printStatus)
            puts("state=stopped");
        return 0;
    }
    if (pidOut != NULL)
        *pidOut = pid;
    if (controlGetStatus(config->endpoint,
                               config->connectTimeoutSeconds,
                               &control, error, sizeof(error)) != 0) {
        if (printStatus) {
            puts("state=starting");
            printf("endpoint=%s\n", config->endpoint);
            printf("pid=%ld\n", (long)pid);
        }
        return 1;
    }
    if (printStatus) {
        puts("state=running");
        printf("endpoint=%s\n", config->endpoint);
        printf("pid=%ld\n", (long)pid);
        printf("running=%u\n", control.running);
        printf("queued=%u\n", control.queued);
        printf("max_running=%u\n", control.maxRunning);
        printf("max_scan_running=%u\n", control.maxScanRunning);
        printf("max_queued=%u\n", control.maxQueued);
    }
    return 2;
}

/*
 * Start or stop server-side performance sampling.
 * @param[in] config: Server connection configuration.
 * @param[in] argument: Sample period in seconds or "stop".
 * @return: 0 on success, 1 on communication failure, or 2 for bad input.
 */
static int
runPerfmonCommand(const struct config *config, const char *argument)
{
    char error[512];
    char *end;
    unsigned long periodSeconds;

    if (strcmp(argument, "stop") == 0) {
        if (controlPerfmonStop(
                config->endpoint,
                config->connectTimeoutSeconds,
                error, sizeof(error)) != 0) {
            fprintf(stderr, "bhist-speedup-server: %s\n", error);
            return 1;
        }
        puts("Performance metric sampling stopped.");
        return 0;
    }
    errno = 0;
    periodSeconds = strtoul(argument, &end, 10);
    if (errno != 0 || end == argument || *end != '\0' ||
        periodSeconds < PERFMON_MIN_PERIOD_SECONDS ||
        periodSeconds > PERFMON_MAX_PERIOD_SECONDS) {
        fprintf(stderr,
                "bhist-speedup-server: sample period must be an integer between "
                "%u and %u seconds\n",
                PERFMON_MIN_PERIOD_SECONDS,
                PERFMON_MAX_PERIOD_SECONDS);
        return 2;
    }
    if (controlPerfmonStart(
            config->endpoint,
            config->connectTimeoutSeconds,
            (unsigned)periodSeconds, error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return 1;
    }
    printf("Performance metric sampling started. Sample period: "
           "%lu Second(s).\n", periodSeconds);
    return 0;
}

/*
 * Read and print the latest complete server performance period.
 * @param[in] config: Server connection configuration.
 * @return: 0 on success, otherwise 1 on communication failure.
 */
static int
viewPerformance(const struct config *config)
{
    struct performanceResult result;
    char error[512];

    if (controlGetPerformance(
            config->endpoint,
            config->connectTimeoutSeconds,
            &result, error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        return 1;
    }
    renderPerformance(stdout, &result);
    return 0;
}

/*
 * Dispatch a server management command.
 * @param[in] argc: Number of command-line arguments.
 * @param[in] argv: Command-line argument vector.
 * @return: 0 on success, 2 for invalid arguments, or another nonzero value
 *          on failure.
 */
int
main(int argc, char **argv)
{
    struct daemonContext context;
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
    if (!((argc == 2 &&
           (strcmp(argv[1], "start") == 0 ||
            strcmp(argv[1], "stop") == 0 ||
            strcmp(argv[1], "status") == 0 ||
            strcmp(argv[1], "view") == 0)) ||
          (argc == 3 && strcmp(argv[1], "perfmon") == 0))) {
        usage(stderr);
        return 2;
    }
    command = argv[1];

    memset(&context, 0, sizeof(context));
    if (configLoad(CONFIG_ROLE_SERVER, &context.config,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        goto done;
    }
    if (configLoadAdmins(&context.config, error, sizeof(error)) != 0 ||
        checkAdminUser(&context.config, callerUid,
                       error, sizeof(error)) != 0 ||
        configRequireServiceHost(&context.config,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        goto done;
    }
    /* Keep root for inspection and stopping services owned by any admin. */
    if (callerUid == 0 && strcmp(command, "stop") != 0 &&
        strcmp(command, "status") != 0 &&
        switchServiceUser(context.config.admins[0], error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        goto done;
    }
    if (strcmp(command, "perfmon") == 0) {
        result = runPerfmonCommand(&context.config, argv[2]);
        goto done;
    }
    if (strcmp(command, "view") == 0) {
        result = viewPerformance(&context.config);
        goto done;
    }
    if (buildPidPath(&context.config, pidPath, sizeof(pidPath),
                        error, sizeof(error)) != 0 ||
        configLogPath(&context.config, "bhist-speedup-server",
                            logPath, sizeof(logPath),
                            error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-server: %s\n", error);
        goto done;
    }
    if (strcmp(command, "start") == 0)
        result = startServer(&context, pidPath, logPath);
    else if (strcmp(command, "stop") == 0)
        result = stopServer(pidPath);
    else
        result = serverStatus(&context.config, pidPath, 1, NULL) > 0 ? 0 : 1;

done:
    configFree(&context.config);
    return result;
}

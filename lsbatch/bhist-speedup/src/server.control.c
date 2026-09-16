#include "server.h"
#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Management-protocol client and text renderer. Each operation uses a new
 * connection for one request. Rendering consumes a decoded snapshot, so text
 * changes do not alter the wire protocol.
 */

#define PERFORMANCE_SEPARATOR \
    "------------------------------------------------------------------------------"

/*
 * Format a management-command error when an output buffer is available.
 * @param[out] error: Optional buffer receiving the formatted message.
 * @param[in] errorSize: Size of error.
 * @param[in] format: printf-style message format followed by its arguments.
 * @return: None.
 */
static void
setError(char *error, size_t errorSize, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || errorSize == 0)
        return;
    va_start(arguments, format);
    (void)vsnprintf(error, errorSize, format, arguments);
    va_end(arguments);
}

/*
 * Decode one 32-bit integer from a management response payload.
 * @param[in] payload: Four-byte network-order source.
 * @return: Host-order value.
 */
static uint32_t
readU32(const unsigned char *payload)
{
    uint32_t value;

    memcpy(&value, payload, sizeof(value));
    return ntohl(value);
}

/*
 * Encode one 32-bit integer into a management request payload.
 * @param[out] payload: Four-byte destination.
 * @param[in] value: Host-order value.
 * @return: None.
 */
static void
writeU32(unsigned char *payload, uint32_t value)
{
    value = htonl(value);
    memcpy(payload, &value, sizeof(value));
}

/*
 * Send one management request and translate transport errors to text.
 * @param[in] fd: Connected service socket.
 * @param[in] type: Protocol frame type.
 * @param[in] payload: Optional frame payload.
 * @param[in] payloadLength: Payload size in bytes.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
sendRequestFrame(int fd, uint8_t type,
                   const void *payload, uint32_t payloadLength,
                   char *error, size_t errorSize)
{
    int result;

    result = sendFrame(fd, type, payload, payloadLength);
    if (result != 0)
        setError(error, errorSize,
                  "cannot send request: %s", strerror(errno));
    return result;
}

/*
 * Decode a server error frame or report an invalid management response.
 * @param[in] operation: Operation name used in diagnostics.
 * @param[in] type: Received frame type.
 * @param[in] payload: Received payload.
 * @param[in] payloadLength: Payload size in bytes.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: None.
 */
static void
setResponseError(const char *operation, uint8_t type,
                   const unsigned char *payload, uint32_t payloadLength,
                   char *error, size_t errorSize)
{
    if (type == FRAME_ERROR && payloadLength >= 4) {
        setError(error, errorSize, "server %s error: %.*s", operation,
                  (int)(payloadLength - 4), (const char *)(payload + 4));
    } else {
        setError(error, errorSize, "invalid server %s response", operation);
    }
}

/*
 * Query current service load and concurrency limits.
 * @param[in] endpoint: Service endpoint.
 * @param[in] connectTimeoutSeconds: Connection timeout in seconds.
 * @param[out] status: Returned service status.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
controlGetStatus(const char *endpoint, int connectTimeoutSeconds,
                       struct controlStatus *status,
                       char *error, size_t errorSize)
{
    unsigned char *payload;
    uint32_t payloadLength;
    uint8_t type;
    int fd;
    int rc;

    if (status == NULL)
        return -1;
    memset(status, 0, sizeof(*status));
    fd = connectEndpoint(endpoint, connectTimeoutSeconds);
    if (fd < 0) {
        setError(error, errorSize, "cannot connect to %s: %s", endpoint,
                  strerror(errno));
        return -1;
    }
    if (sendRequestFrame(fd, FRAME_STATUS, NULL, 0,
                           error, errorSize) != 0) {
        close(fd);
        return -1;
    }
    payload = NULL;
    payloadLength = 0;
    rc = receiveFrame(fd, &type, &payload, &payloadLength,
                        PROTOCOL_DATA_MAX);
    close(fd);
    if (rc != 0) {
        setError(error, errorSize, "server closed the status connection");
        free(payload);
        return -1;
    }
    if (type == FRAME_STATUS_RESULT && payloadLength == 20) {
        status->running = readU32(payload);
        status->queued = readU32(payload + 4);
        status->maxRunning = readU32(payload + 8);
        status->maxScanRunning = readU32(payload + 12);
        status->maxQueued = readU32(payload + 16);
        free(payload);
        return 0;
    }
    setResponseError("status", type, payload, payloadLength,
                       error, errorSize);
    free(payload);
    return -1;
}

/*
 * Send a perfmon start or stop request and validate the resulting state.
 * @param[in] endpoint: Service endpoint.
 * @param[in] connectTimeoutSeconds: Connection timeout in seconds.
 * @param[in] requestType: PERFMON_START or PERFMON_STOP frame type.
 * @param[in] requestPayload: Optional request payload.
 * @param[in] requestLength: Payload size in bytes.
 * @param[in] wantedStarted: Expected enabled state in the response.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
requestPerfmon(const char *endpoint, int connectTimeoutSeconds,
                        uint8_t requestType, const void *requestPayload,
                        uint32_t requestLength, uint32_t wantedStarted,
                        char *error, size_t errorSize)
{
    unsigned char *payload;
    uint32_t payloadLength;
    uint8_t type;
    int fd;
    int rc;

    fd = connectEndpoint(endpoint, connectTimeoutSeconds);
    if (fd < 0) {
        setError(error, errorSize, "cannot connect to %s: %s", endpoint,
                  strerror(errno));
        return -1;
    }
    if (sendRequestFrame(fd, requestType,
                           requestPayload, requestLength,
                           error, errorSize) != 0) {
        close(fd);
        return -1;
    }
    payload = NULL;
    payloadLength = 0;
    rc = receiveFrame(fd, &type, &payload, &payloadLength,
                        PROTOCOL_DATA_MAX);
    close(fd);
    if (rc != 0) {
        setError(error, errorSize,
                  "server closed the performance monitor connection");
        free(payload);
        return -1;
    }
    if (type == FRAME_PERFMON_RESULT && payloadLength == 4 &&
        readU32(payload) == wantedStarted) {
        free(payload);
        return 0;
    }
    setResponseError("performance monitor", type, payload, payloadLength,
                       error, errorSize);
    free(payload);
    return -1;
}

/*
 * Start service performance sampling.
 * @param[in] endpoint: Service endpoint.
 * @param[in] connectTimeoutSeconds: Connection timeout in seconds.
 * @param[in] periodSeconds: Sample period in seconds.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
controlPerfmonStart(const char *endpoint, int connectTimeoutSeconds,
                          unsigned periodSeconds,
                          char *error, size_t errorSize)
{
    unsigned char payload[4];

    if (periodSeconds < PERFMON_MIN_PERIOD_SECONDS ||
        periodSeconds > PERFMON_MAX_PERIOD_SECONDS) {
        setError(error, errorSize,
                  "sample period must be between %u and %u seconds",
                  PERFMON_MIN_PERIOD_SECONDS,
                  PERFMON_MAX_PERIOD_SECONDS);
        return -1;
    }
    writeU32(payload, periodSeconds);
    return requestPerfmon(
        endpoint, connectTimeoutSeconds, FRAME_PERFMON_START,
        payload, sizeof(payload), 1, error, errorSize);
}

/*
 * Stop service performance sampling.
 * @param[in] endpoint: Service endpoint.
 * @param[in] connectTimeoutSeconds: Connection timeout in seconds.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
controlPerfmonStop(const char *endpoint, int connectTimeoutSeconds,
                         char *error, size_t errorSize)
{
    return requestPerfmon(
        endpoint, connectTimeoutSeconds, FRAME_PERFMON_STOP,
        NULL, 0, 0, error, errorSize);
}

/*
 * Read the latest service performance snapshot.
 * @param[in] endpoint: Service endpoint.
 * @param[in] connectTimeoutSeconds: Connection timeout in seconds.
 * @param[out] result: Returned performance snapshot.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
controlGetPerformance(const char *endpoint, int connectTimeoutSeconds,
                            struct performanceResult *result,
                            char *error, size_t errorSize)
{
    unsigned char *payload;
    uint32_t payloadLength;
    uint8_t type;
    int fd;
    int rc;

    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    fd = connectEndpoint(endpoint, connectTimeoutSeconds);
    if (fd < 0) {
        setError(error, errorSize, "cannot connect to %s: %s", endpoint,
                  strerror(errno));
        return -1;
    }
    if (sendRequestFrame(fd, FRAME_PERFORMANCE_VIEW,
                           NULL, 0, error, errorSize) != 0) {
        close(fd);
        return -1;
    }
    payload = NULL;
    payloadLength = 0;
    rc = receiveFrame(fd, &type, &payload, &payloadLength,
                        PROTOCOL_DATA_MAX);
    close(fd);
    if (rc != 0) {
        setError(error, errorSize,
                  "server closed the performance view connection");
        free(payload);
        return -1;
    }
    if (type == FRAME_PERFORMANCE_RESULT &&
        decodePerformanceResult(payload, payloadLength, result) == 0) {
        free(payload);
        return 0;
    }
    setResponseError("performance view", type, payload, payloadLength,
                       error, errorSize);
    free(payload);
    return -1;
}

/*
 * Format a byte count using a compact binary unit.
 * @param[in] bytes: Byte count to format.
 * @param[out] buffer: Buffer receiving human-readable text.
 * @param[in] bufferSize: Size of buffer.
 * @return: None.
 */
static void
formatBytes(double bytes, char *buffer, size_t bufferSize)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t unit;

    unit = 0;
    while (bytes >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        bytes /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buffer, bufferSize, "%.0f %s", bytes, units[unit]);
    else
        snprintf(buffer, bufferSize, "%.1f %s", bytes, units[unit]);
}

/*
 * Format an epoch timestamp in local time or use "---" when unavailable.
 * @param[in] seconds: Unix timestamp.
 * @param[out] buffer: Buffer receiving formatted text.
 * @param[in] bufferSize: Size of buffer.
 * @return: None.
 */
static void
formatTimestamp(uint64_t seconds, char *buffer, size_t bufferSize)
{
    struct tm localTime;
    time_t value;

    value = (time_t)seconds;
    if ((uint64_t)value != seconds ||
        localtime_r(&value, &localTime) == NULL ||
        strftime(buffer, bufferSize, "%a %b %d %H:%M:%S",
                 &localTime) == 0) {
        snprintf(buffer, bufferSize, "---");
    }
}

/*
 * Format a per-period average or "-" when no sample exists.
 * @param[in] sum: Lifetime metric total.
 * @param[in] count: Completed sample periods.
 * @param[out] buffer: Buffer receiving formatted text.
 * @param[in] bufferSize: Size of buffer.
 * @return: None.
 */
static void
formatAverage(uint64_t sum, uint64_t count, char *buffer,
               size_t bufferSize)
{
    if (count == 0)
        snprintf(buffer, bufferSize, "-");
    else
        snprintf(buffer, bufferSize, "%.1f", (double)sum / (double)count);
}

/*
 * Format a period maximum or "-" when no sample exists.
 * @param[in] maximum: Largest completed-period value.
 * @param[in] count: Completed sample periods.
 * @param[out] buffer: Buffer receiving formatted text.
 * @param[in] bufferSize: Size of buffer.
 * @return: None.
 */
static void
formatMaximum(uint64_t maximum, uint64_t count, char *buffer,
               size_t bufferSize)
{
    if (count == 0)
        snprintf(buffer, bufferSize, "-");
    else
        snprintf(buffer, bufferSize, "%" PRIu64, maximum);
}

/*
 * Render a performance snapshot as text.
 * @param[out] stream: Destination stream.
 * @param[in] result: Performance snapshot.
 * @return: None.
 */
void
renderPerformance(FILE *stream,
                       const struct performanceResult *result)
{
    static const char *const requestNames[] = {
        "job_id", "time_range", "history_scan", "TOTAL"
    };
    char startTime[64];
    char endTime[64];
    char maxBuffer[32];
    char averageBuffer[32];
    char lastBytes[32];
    char maxBytes[32];
    char averageBytes[32];
    char totalBytes[32];
    size_t i;

    if (stream == NULL || result == NULL)
        return;
    if (!result->monitorStarted) {
        fputs("Performance metric sampling has not been started.\n", stream);
        return;
    }

    formatTimestamp(result->monitorStartTime, startTime,
                     sizeof(startTime));
    fprintf(stream, "Performance monitor start time: %s\n", startTime);
    if (result->completePeriodCount == 0) {
        snprintf(endTime, sizeof(endTime), "---");
    } else {
        formatTimestamp(result->lastSampleEndTime, endTime,
                         sizeof(endTime));
    }
    fprintf(stream, "End time of last sample period: %s\n", endTime);
    fprintf(stream, "Sample period:                  %u Second(s)\n",
            result->samplePeriodSeconds);
    fputs(PERFORMANCE_SEPARATOR "\n", stream);
    if (result->completePeriodCount == 0) {
        fprintf(stream,
                "No performance metric data available. Please wait until "
                "first sample period ends in %u second(s).\n",
                result->secondsUntilFirstSample);
        return;
    }

    fputs("REQUESTS\n", stream);
    fprintf(stream, "  %-16s %12s %12s %12s %12s\n",
            "Type", "Last", "Max", "Avg", "Total");
    for (i = 0; i < PERFORMANCE_REQUEST_ROWS; i++) {
        formatMaximum(result->requests[i].max,
                       result->completePeriodCount,
                       maxBuffer, sizeof(maxBuffer));
        formatAverage(result->requests[i].total,
                       result->completePeriodCount,
                       averageBuffer, sizeof(averageBuffer));
        fprintf(stream,
                "  %-16s %12" PRIu64 " %12s %12s %12" PRIu64 "\n",
                requestNames[i], result->requests[i].last,
                maxBuffer, averageBuffer, result->requests[i].total);
    }

    fputs(PERFORMANCE_SEPARATOR "\n", stream);
    fputs("OUTPUT TO CLIENT\n", stream);
    fprintf(stream, "  %-16s %12s %12s %12s %12s\n",
            "Type", "Last", "Max", "Avg", "Total");
    formatBytes((double)result->output.last, lastBytes,
                 sizeof(lastBytes));
    formatBytes((double)result->output.max, maxBytes,
                 sizeof(maxBytes));
    formatBytes((double)result->output.total /
                 (double)result->completePeriodCount,
                 averageBytes, sizeof(averageBytes));
    formatBytes((double)result->output.total, totalBytes,
                 sizeof(totalBytes));
    fprintf(stream, "  %-16s %12s %12s %12s %12s\n",
            "TOTAL", lastBytes, maxBytes, averageBytes, totalBytes);
    fputs(PERFORMANCE_SEPARATOR "\n", stream);
}

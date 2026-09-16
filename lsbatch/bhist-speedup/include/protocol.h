#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PROTOCOL_VERSION 5
#define PROTOCOL_HEADER_SIZE 8
#define PROTOCOL_REQUEST_MAX (64U * 1024U)
#define PROTOCOL_DATA_MAX (32U * 1024U)
#define PROTOCOL_ARGC_MAX 64
#define PROTOCOL_ARG_MAX 4096U
#define PROTOCOL_USER_HINT_MAX 255U
#define PERFORMANCE_PROTOCOL_VERSION 4U
#define PERFORMANCE_REQUEST_ROWS 4
#define PERFMON_MIN_PERIOD_SECONDS 1U
#define PERFMON_MAX_PERIOD_SECONDS 86400U

/* Identifies each request, stream, control, and response frame on the wire. */
enum frameType {
    FRAME_QUERY = 1,
    FRAME_STATUS = 2,
    FRAME_PERFMON_START = 3,
    FRAME_PERFMON_STOP = 4,
    FRAME_PERFORMANCE_VIEW = 5,
    FRAME_STDOUT = 18,
    FRAME_STDERR = 19,
    FRAME_EXIT = 20,
    FRAME_ERROR = 21,
    FRAME_STATUS_RESULT = 22,
    FRAME_PERFORMANCE_RESULT = 23,
    FRAME_PERFMON_RESULT = 24
};

/* Stable server error categories returned in FRAME_ERROR frames. */
enum errorCode {
    SERVICE_ERROR_PROTOCOL = 1,
    SERVICE_ERROR_BUSY = 2,
    SERVICE_ERROR_INTERNAL = 3,
    SERVICE_ERROR_SHUTDOWN = 4
};

/* Owns a decoded query request and its bhist-compatible argument vector. */
struct queryRequest {
    char *userHint;
    int argc;
    char **argv;
};

/* Wire representation of last, maximum, and total metric values. */
struct performanceMetric {
    uint64_t last;
    uint64_t max;
    uint64_t total;
};

/* Wire representation of the latest completed perfmon period. */
struct performanceResult {
    uint32_t monitorStarted;
    uint32_t samplePeriodSeconds;
    uint32_t secondsUntilFirstSample;
    uint64_t monitorStartTime;
    uint64_t lastSampleEndTime;
    uint64_t completePeriodCount;
    struct performanceMetric
        requests[PERFORMANCE_REQUEST_ROWS];
    struct performanceMetric output;
};

int sendFrame(int fd, uint8_t type, const void *payload,
              uint32_t payloadLength);

int receiveFrame(int fd, uint8_t *typeOut, unsigned char **payloadOut,
                 uint32_t *payloadLengthOut, uint32_t maxPayloadLength);

int encodeQueryRequest(const struct queryRequest *request,
                       unsigned char **payloadOut,
                       uint32_t *payloadLengthOut);

int decodeQueryRequest(const unsigned char *payload,
                       uint32_t payloadLength,
                       struct queryRequest *requestOut);

void freeQueryRequest(struct queryRequest *request);

int encodePerformanceResult(
    const struct performanceResult *result,
    unsigned char **payloadOut, uint32_t *payloadLengthOut);

int decodePerformanceResult(
    const unsigned char *payload, uint32_t payloadLength,
    struct performanceResult *resultOut);

#endif

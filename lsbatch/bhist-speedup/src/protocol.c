#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/*
 * Protocol framing and payload serialization. This module performs neither
 * connection setup nor command authorization. All multibyte integers use
 * network byte order, and protocol.h defines payload ownership and limits.
 */
static int receiveExact(int fd, void *buffer, size_t length);
static int sendAllSocket(int fd, const void *data, size_t length);
static void writeU32(unsigned char *buffer, uint32_t value);
static uint32_t readU32(const unsigned char *buffer);

/*
 * Send an entire byte range to a connected socket.
 * @param[in] fd: Connected socket descriptor.
 * @param[in] data: Bytes to send.
 * @param[in] length: Number of bytes to send.
 * @return: 0 on success, otherwise -1.
 */
static int
sendAllSocket(int fd, const void *data, size_t length)
{
    const unsigned char *cursor;
    ssize_t written;
    int flags;

    cursor = (const unsigned char *)data;
    flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    while (length > 0) {
        written = send(fd, cursor, length, flags);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}
/*
 * Receive an exact byte count while distinguishing clean initial EOF.
 * @param[in] fd: Connected socket descriptor.
 * @param[out] buffer: Buffer receiving bytes.
 * @param[in] length: Required byte count.
 * @return: 0 on success, 1 for EOF before any byte, otherwise -1.
 */
static int
receiveExact(int fd, void *buffer, size_t length)
{
    unsigned char *cursor;
    size_t received;
    ssize_t rc;

    cursor = (unsigned char *)buffer;
    received = 0;
    while (received < length) {
        rc = recv(fd, cursor + received, length - received, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return received == 0 ? 1 : -1;
        received += (size_t)rc;
    }
    return 0;
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
    uint32_t networkValue;

    memcpy(&networkValue, buffer, sizeof(networkValue));
    return ntohl(networkValue);
}

/*
 * Encode one 64-bit integer in network byte order.
 * @param[out] buffer: Eight-byte destination.
 * @param[in] value: Host-order value.
 * @return: None.
 */
static void
writeU64(unsigned char *buffer, uint64_t value)
{
    writeU32(buffer, (uint32_t)(value >> 32));
    writeU32(buffer + 4, (uint32_t)(value & 0xffffffffU));
}

/*
 * Decode one 64-bit network-order integer.
 * @param[in] buffer: Eight-byte source.
 * @return: Host-order value.
 */
static uint64_t
readU64(const unsigned char *buffer)
{
    return ((uint64_t)readU32(buffer) << 32) |
        (uint64_t)readU32(buffer + 4);
}

/*
 * Send one protocol frame.
 * @param[in] fd: Socket descriptor.
 * @param[in] type: Frame type.
 * @param[in] payload: Frame payload.
 * @param[in] payloadLength: Payload size in bytes.
 * @return: 0 on success, otherwise -1.
 */
int
sendFrame(int fd, uint8_t type, const void *payload,
               uint32_t payloadLength)
{
    unsigned char header[PROTOCOL_HEADER_SIZE];

    if (payloadLength > 0 && payload == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(header, 0, sizeof(header));
    header[0] = PROTOCOL_VERSION;
    header[1] = type;
    writeU32(header + 4, payloadLength);
    if (sendAllSocket(fd, header, sizeof(header)) != 0)
        return -1;
    if (payloadLength > 0 &&
        sendAllSocket(fd, payload, payloadLength) != 0)
        return -1;
    return 0;
}

/*
 * Receive and validate one protocol frame.
 * @param[in] fd: Socket descriptor.
 * @param[out] typeOut: Received frame type.
 * @param[out] payloadOut: Newly allocated payload, or NULL for an empty frame.
 * @param[out] payloadLengthOut: Payload size in bytes.
 * @param[in] maxPayloadLength: Maximum accepted payload size.
 * @return: 0 on success, 1 for EOF before a frame, otherwise -1.
 */
int
receiveFrame(int fd, uint8_t *typeOut, unsigned char **payloadOut,
               uint32_t *payloadLengthOut, uint32_t maxPayloadLength)
{
    unsigned char header[PROTOCOL_HEADER_SIZE];
    unsigned char *payload;
    uint32_t payloadLength;
    int rc;

    if (typeOut == NULL || payloadOut == NULL ||
        payloadLengthOut == NULL) {
        errno = EINVAL;
        return -1;
    }
    *payloadOut = NULL;
    *payloadLengthOut = 0;

    rc = receiveExact(fd, header, sizeof(header));
    if (rc != 0)
        return rc;
    if (header[0] != PROTOCOL_VERSION || header[2] != 0 ||
        header[3] != 0) {
        errno = EPROTO;
        return -1;
    }
    payloadLength = readU32(header + 4);
    if (payloadLength > maxPayloadLength) {
        errno = EMSGSIZE;
        return -1;
    }
    payload = NULL;
    if (payloadLength > 0) {
        payload = (unsigned char *)malloc((size_t)payloadLength + 1);
        if (payload == NULL)
            return -1;
        rc = receiveExact(fd, payload, payloadLength);
        if (rc != 0) {
            free(payload);
            if (rc == 1)
                errno = EPROTO;
            return -1;
        }
        payload[payloadLength] = '\0';
    }

    *typeOut = header[1];
    *payloadOut = payload;
    *payloadLengthOut = payloadLength;
    return 0;
}

#define PERFORMANCE_HEADER_SIZE 40U
#define PERFORMANCE_METRIC_SIZE 24U
#define PERFORMANCE_PAYLOAD_SIZE \
    (PERFORMANCE_HEADER_SIZE + \
     (PERFORMANCE_REQUEST_ROWS + 1U) * \
         PERFORMANCE_METRIC_SIZE)

/*
 * Encode one performance metric into its fixed-size wire representation.
 * @param[out] payload: Destination metric payload.
 * @param[in] metric: Host representation to encode.
 * @return: None.
 */
static void
encodePerformanceMetric(unsigned char *payload,
                        const struct performanceMetric *metric)
{
    writeU64(payload, metric->last);
    writeU64(payload + 8, metric->max);
    writeU64(payload + 16, metric->total);
}

/*
 * Decode one fixed-size wire metric.
 * @param[in] payload: Source metric payload.
 * @param[out] metric: Host representation receiving values.
 * @return: None.
 */
static void
decodePerformanceMetric(const unsigned char *payload,
                        struct performanceMetric *metric)
{
    metric->last = readU64(payload);
    metric->max = readU64(payload + 8);
    metric->total = readU64(payload + 16);
}

/*
 * Encode a performance result payload.
 * @param[in] result: Result to encode.
 * @param[out] payloadOut: Newly allocated payload.
 * @param[out] payloadLengthOut: Payload size in bytes.
 * @return: 0 on success, otherwise -1.
 */
int
encodePerformanceResult(const struct performanceResult *result,
                              unsigned char **payloadOut,
                              uint32_t *payloadLengthOut)
{
    unsigned char *payload;
    size_t offset;
    int i;

    if (result == NULL || payloadOut == NULL ||
        payloadLengthOut == NULL)
        return -1;
    *payloadOut = NULL;
    *payloadLengthOut = 0;
    payload = (unsigned char *)malloc(PERFORMANCE_PAYLOAD_SIZE);
    if (payload == NULL)
        return -1;

    writeU32(payload, PERFORMANCE_PROTOCOL_VERSION);
    writeU32(payload + 4, result->monitorStarted);
    writeU32(payload + 8, result->samplePeriodSeconds);
    writeU32(payload + 12, result->secondsUntilFirstSample);
    writeU64(payload + 16, result->monitorStartTime);
    writeU64(payload + 24, result->lastSampleEndTime);
    writeU64(payload + 32, result->completePeriodCount);
    offset = PERFORMANCE_HEADER_SIZE;
    for (i = 0; i < PERFORMANCE_REQUEST_ROWS; i++) {
        encodePerformanceMetric(payload + offset, &result->requests[i]);
        offset += PERFORMANCE_METRIC_SIZE;
    }
    encodePerformanceMetric(payload + offset, &result->output);
    *payloadOut = payload;
    *payloadLengthOut = PERFORMANCE_PAYLOAD_SIZE;
    return 0;
}

/*
 * Decode a performance result payload.
 * @param[in] payload: Payload to decode.
 * @param[in] payloadLength: Payload size in bytes.
 * @param[out] resultOut: Decoded result.
 * @return: 0 on success, otherwise -1.
 */
int
decodePerformanceResult(const unsigned char *payload,
                              uint32_t payloadLength,
                              struct performanceResult *resultOut)
{
    size_t offset;
    int i;

    if (payload == NULL || resultOut == NULL ||
        payloadLength != PERFORMANCE_PAYLOAD_SIZE ||
        readU32(payload) != PERFORMANCE_PROTOCOL_VERSION)
        return -1;
    memset(resultOut, 0, sizeof(*resultOut));
    resultOut->monitorStarted = readU32(payload + 4);
    resultOut->samplePeriodSeconds = readU32(payload + 8);
    resultOut->secondsUntilFirstSample = readU32(payload + 12);
    resultOut->monitorStartTime = readU64(payload + 16);
    resultOut->lastSampleEndTime = readU64(payload + 24);
    resultOut->completePeriodCount = readU64(payload + 32);
    offset = PERFORMANCE_HEADER_SIZE;
    for (i = 0; i < PERFORMANCE_REQUEST_ROWS; i++) {
        decodePerformanceMetric(payload + offset, &resultOut->requests[i]);
        offset += PERFORMANCE_METRIC_SIZE;
    }
    decodePerformanceMetric(payload + offset, &resultOut->output);
    return 0;
}

/*
 * Encode a query request payload.
 * @param[in] request: Request to encode.
 * @param[out] payloadOut: Newly allocated payload.
 * @param[out] payloadLengthOut: Payload size in bytes.
 * @return: 0 on success, otherwise -1.
 */
int
encodeQueryRequest(const struct queryRequest *request,
                         unsigned char **payloadOut,
                         uint32_t *payloadLengthOut)
{
    unsigned char *payload;
    size_t total;
    size_t offset;
    size_t length;
    int i;

    if (request == NULL || payloadOut == NULL ||
        payloadLengthOut == NULL || request->userHint == NULL ||
        request->argc < 0 || request->argc > PROTOCOL_ARGC_MAX ||
        (request->argc > 0 && request->argv == NULL))
        return -1;
    *payloadOut = NULL;
    *payloadLengthOut = 0;

    length = strlen(request->userHint);
    if (length == 0 || length > PROTOCOL_USER_HINT_MAX)
        return -1;
    total = 8 + length;
    for (i = 0; i < request->argc; i++) {
        if (request->argv[i] == NULL)
            return -1;
        length = strlen(request->argv[i]);
        if (length > PROTOCOL_ARG_MAX || total > SIZE_MAX - 4 - length)
            return -1;
        total += 4 + length;
    }
    if (total > PROTOCOL_REQUEST_MAX)
        return -1;

    payload = (unsigned char *)malloc(total);
    if (payload == NULL)
        return -1;
    offset = 0;
    length = strlen(request->userHint);
    writeU32(payload + offset, (uint32_t)length);
    offset += 4;
    memcpy(payload + offset, request->userHint, length);
    offset += length;
    writeU32(payload + offset, (uint32_t)request->argc);
    offset += 4;
    for (i = 0; i < request->argc; i++) {
        length = strlen(request->argv[i]);
        writeU32(payload + offset, (uint32_t)length);
        offset += 4;
        memcpy(payload + offset, request->argv[i], length);
        offset += length;
    }

    *payloadOut = payload;
    *payloadLengthOut = (uint32_t)total;
    return 0;
}

/*
 * Decode a query request payload.
 * @param[in] payload: Payload to decode.
 * @param[in] payloadLength: Payload size in bytes.
 * @param[out] requestOut: Decoded request.
 * @return: 0 on success, otherwise -1.
 */
int
decodeQueryRequest(const unsigned char *payload,
                         uint32_t payloadLength,
                         struct queryRequest *requestOut)
{
    uint32_t argc;
    uint32_t userHintLength;
    uint32_t length;
    size_t offset;
    uint32_t i;

    if (payload == NULL || requestOut == NULL || payloadLength < 8 ||
        payloadLength > PROTOCOL_REQUEST_MAX)
        return -1;
    memset(requestOut, 0, sizeof(*requestOut));

    offset = 0;
    userHintLength = readU32(payload + offset);
    offset += 4;
    if (userHintLength == 0 ||
        userHintLength > PROTOCOL_USER_HINT_MAX ||
        offset + userHintLength + 4 > payloadLength)
        goto fail;
    requestOut->userHint = (char *)malloc((size_t)userHintLength + 1);
    if (requestOut->userHint == NULL)
        goto fail;
    memcpy(requestOut->userHint, payload + offset, userHintLength);
    requestOut->userHint[userHintLength] = '\0';
    if (memchr(requestOut->userHint, '\0', userHintLength) != NULL)
        goto fail;
    offset += userHintLength;

    argc = readU32(payload + offset);
    offset += 4;
    if (argc > PROTOCOL_ARGC_MAX)
        goto fail;
    requestOut->argc = (int)argc;
    requestOut->argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (requestOut->argv == NULL)
        goto fail;
    for (i = 0; i < argc; i++) {
        if (offset + 4 > payloadLength)
            goto fail;
        length = readU32(payload + offset);
        offset += 4;
        if (length > PROTOCOL_ARG_MAX || offset + length > payloadLength)
            goto fail;
        requestOut->argv[i] = (char *)malloc((size_t)length + 1);
        if (requestOut->argv[i] == NULL)
            goto fail;
        memcpy(requestOut->argv[i], payload + offset, length);
        requestOut->argv[i][length] = '\0';
        if (memchr(requestOut->argv[i], '\0', length) != NULL)
            goto fail;
        offset += length;
    }
    if (offset != payloadLength)
        goto fail;
    return 0;

fail:
    freeQueryRequest(requestOut);
    return -1;
}

/*
 * Release storage owned by a decoded query request.
 * @param[in,out] request: Request to release and reset.
 * @return: None.
 */
void
freeQueryRequest(struct queryRequest *request)
{
    int i;

    if (request == NULL)
        return;
    if (request->argv != NULL) {
        for (i = 0; i < request->argc; i++)
            free(request->argv[i]);
    }
    free(request->argv);
    free(request->userHint);
    memset(request, 0, sizeof(*request));
}

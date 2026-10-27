/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */

#include "bhist.util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Format an error message when the caller supplied an output buffer.
 * @param[out] error: Optional buffer receiving the formatted message.
 * @param[in] errorSize: Size of error.
 * @param[in] format: printf-style message format followed by its arguments.
 * @return: None.
 */
void
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
 * Encode one 32-bit host-order integer as network-order bytes.
 * @param[out] buffer: Four-byte destination.
 * @param[in] value: Host-order value.
 * @return: None.
 */
void
writeU32(unsigned char *buffer, uint32_t value)
{
    uint32_t networkValue;

    networkValue = htonl(value);
    memcpy(buffer, &networkValue, sizeof(networkValue));
}

/*
 * Sleep for a millisecond duration while retrying interrupted nanosleep calls.
 * @param[in] milliseconds: Requested sleep duration.
 * @return: None.
 */
void
sleepMs(int milliseconds)
{
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
        ;
}

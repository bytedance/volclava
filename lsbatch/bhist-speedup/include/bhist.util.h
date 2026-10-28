/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 *
 * Shared helpers used by every bhist speedup program.
 */
#ifndef BHIST_UTIL_H
#define BHIST_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Format an error message when the caller supplied an output buffer.
 * @param[out] error: Optional buffer receiving the formatted message.
 * @param[in] errorSize: Size of error.
 * @param[in] format: printf-style message format followed by its arguments.
 * @return: None.
 */
void setError(char *error, size_t errorSize, const char *format, ...);

/*
 * Encode one 32-bit host-order integer as network-order bytes.
 * @param[out] buffer: Four-byte destination.
 * @param[in] value: Host-order value.
 * @return: None.
 */
void writeU32(unsigned char *buffer, uint32_t value);

/*
 * Sleep for a millisecond duration while retrying interrupted nanosleep calls.
 * @param[in] milliseconds: Requested sleep duration.
 * @return: None.
 */
void sleepMs(int milliseconds);

#endif /* BHIST_UTIL_H */

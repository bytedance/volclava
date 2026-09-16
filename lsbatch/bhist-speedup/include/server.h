#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"

#include <stddef.h>
#include <stdio.h>

/* Holds service load and configured concurrency limits returned by status. */
struct controlStatus {
    unsigned running;
    unsigned queued;
    unsigned maxRunning;
    unsigned maxScanRunning;
    unsigned maxQueued;
};

int controlGetStatus(const char *endpoint, int connectTimeoutSeconds,
                     struct controlStatus *status,
                     char *error, size_t errorSize);

int controlPerfmonStart(const char *endpoint, int connectTimeoutSeconds,
                        unsigned periodSeconds,
                        char *error, size_t errorSize);

int controlPerfmonStop(const char *endpoint, int connectTimeoutSeconds,
                       char *error, size_t errorSize);

int controlGetPerformance(const char *endpoint, int connectTimeoutSeconds,
                          struct performanceResult *result,
                          char *error, size_t errorSize);

void renderPerformance(FILE *stream,
                       const struct performanceResult *result);

int runQuery(int argc, char **argv);

#endif

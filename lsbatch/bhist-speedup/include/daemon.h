#ifndef DAEMON_H
#define DAEMON_H

#include <stddef.h>
#include <sys/types.h>

struct config;

int checkAdminUser(const struct config *config, uid_t callerUid,
                   char *error, size_t errorSize);

int switchServiceUser(const char *user, char *error, size_t errorSize);

typedef int (*daemonCallback)(void *context);

int pidFileStatus(const char *path, pid_t *pidOut,
                  char *error, size_t errorSize);

int pidFileRemoveStale(const char *path,
                       char *error, size_t errorSize);

int daemonStart(const char *pidPath, const char *logPath,
                daemonCallback callback, void *context,
                pid_t *pidOut, char *error, size_t errorSize);

int daemonStop(const char *pidPath, int timeoutMs,
               pid_t *pidOut, int *wasRunning,
               char *error, size_t errorSize);

int parentDirectory(const char *path, char *parent, size_t parentSize,
                    char *error, size_t errorSize);

int ensureDirectory(const char *path,
                    char *error, size_t errorSize);

#endif

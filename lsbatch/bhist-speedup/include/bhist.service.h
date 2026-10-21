#ifndef BHIST_SERVICE_H
#define BHIST_SERVICE_H

#include <stddef.h>
#include <sys/types.h>

struct config;

/*
 * Exit statuses shared by the loader and server. A systemd unit lists the
 * statuses that a restart cannot fix in RestartPreventExitStatus.
 */
#define SERVICE_EXIT_FAILURE 1
#define SERVICE_EXIT_USAGE 2
#define SERVICE_EXIT_CONFIG 3
#define SERVICE_EXIT_CLEAN_REQUIRED 4
#define SERVICE_EXIT_RUNNING 5

int checkAdminUser(const struct config *config, uid_t callerUid,
                   char *error, size_t errorSize);

int checkSqliteRuntime(void);

int switchServiceUser(const char *user, char *error, size_t errorSize);

int prepareServicePaths(const char *dbPath, const char *logPath,
                        const char *adminUser,
                        char *error, size_t errorSize);

int findServiceProcess(const char *name, pid_t *pidOut,
                       char *error, size_t errorSize);

void serviceNotify(const char *state);

int parentDirectory(const char *path, char *parent, size_t parentSize,
                    char *error, size_t errorSize);

int ensureDirectory(const char *path,
                    char *error, size_t errorSize);

#endif

#ifndef BHIST_CONFIG_H
#define BHIST_CONFIG_H

#include <stddef.h>

#ifndef CONFIG_PATH_MAX
#define CONFIG_PATH_MAX 4096
#endif

#define SERVICE_ENDPOINT_MAX 512

/* Selects which subset of lsb.hist a process consumes and validates. */
enum configRole {
    CONFIG_ROLE_CLIENT = 1,
    CONFIG_ROLE_SERVER = 2,
    CONFIG_ROLE_LOADER = 4
};

/*
 * Holds the role-filtered runtime configuration inherited from
 * lsb.hist and lsf.conf.
 */
struct config {
    char envDir[CONFIG_PATH_MAX];
    char confDir[CONFIG_PATH_MAX];
    char *clusterName;
    char **admins;
    size_t adminCount;
    char configPath[CONFIG_PATH_MAX];
    char endpoint[SERVICE_ENDPOINT_MAX];
    char dbPath[CONFIG_PATH_MAX];
    char binDir[CONFIG_PATH_MAX];
    char bhistOriginalPath[CONFIG_PATH_MAX];
    char eventDir[CONFIG_PATH_MAX];
    char logDir[CONFIG_PATH_MAX];
    char logMask[64];

    int eventPollIntervalMs;
    long long maxEventsPerTransaction;
    long long maxRawSizePerTransactionMib;
    long long historyRetentionSeconds;
    int importCacheSizeMib;

    int dbBusyTimeoutMs;
    int queryDbCacheSizeMib;
    int queryDbMmapSizeMib;
    int formattersPerQuery;
    int jobsPerBatch;

    int maxConcurrentQueries;
    int maxConcurrentScanQueries;
    int maxQueuedQueries;
    int connectTimeoutSeconds;
    int receiveTimeoutSeconds;
};

/* Initializes a fresh config; call configFree before loading into it again. */
int configLoad(enum configRole role, struct config *config,
               char *error, size_t errorSize);

/* Service commands load administrator information before command dispatch. */
int configLoadAdmins(struct config *config, char *error, size_t errorSize);

/* Release owned strings after a successful or failed load. */
void configFree(struct config *config);

int configRequireServiceHost(const struct config *config,
                             char *error, size_t errorSize);

int configLogPath(const struct config *config, const char *name,
                  char *path, size_t pathSize,
                  char *error, size_t errorSize);

#endif

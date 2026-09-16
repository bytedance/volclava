#include "bhist.config.h"

#include "lib.h"
#include "lproto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Strict, non-shell configuration parser.
 *
 * parseFile() accepts known KEY=VALUE assignments, surrounding whitespace,
 * comments, and one pair of quotes around the complete value. It never
 * sources a file, expands variables, or executes shell code.
 * validateAndAssign() then applies role-specific defaults and range checks,
 * while loadLsfEnvironment() obtains cluster paths through Volclava initenv_.
 *
 * A new setting must update the key enum and name table, defaults, role-based
 * validation, and tests together. Server- or loader-only paths must not be
 * validated for the client role because those paths may be inaccessible on a
 * client node.
 */
/* Indexes the supported lsb.hist keys and keyNames. */
enum configKey {
    KEY_SERVICE_ENDPOINT,
    KEY_DB_PATH,
    KEY_EVENT_DIR,
    KEY_EVENT_POLL_INTERVAL,
    KEY_MAX_EVENTS_PER_TRANSACTION,
    KEY_MAX_RAW_SIZE_PER_TRANSACTION,
    KEY_HISTORY_RETENTION,
    KEY_IMPORT_CACHE_SIZE,
    KEY_DB_BUSY_TIMEOUT,
    KEY_QUERY_DB_CACHE_SIZE,
    KEY_QUERY_DB_MMAP_SIZE,
    KEY_MAX_FORMATTERS_PER_QUERY,
    KEY_JOBS_PER_BATCH,
    KEY_MAX_CONCURRENT_QUERIES,
    KEY_MAX_CONCURRENT_SCAN_QUERIES,
    KEY_MAX_QUEUED_QUERIES,
    KEY_CONNECT_TIMEOUT,
    KEY_RECEIVE_TIMEOUT,
    KEY_COUNT
};

/* Canonical spelling of every accepted lsb.hist key. */
static const char *const keyNames[KEY_COUNT] = {
    "LSB_BHIST_ENDPOINT",
    "LSB_BHIST_DB_PATH",
    "LSB_BHIST_EVENT_DIR",
    "LSB_BHIST_EVENT_POLL_INTERVAL",
    "LSB_BHIST_MAX_EVENTS_PER_TRANSACTION",
    "LSB_BHIST_MAX_RAW_SIZE_PER_TRANSACTION",
    "LSB_BHIST_HISTORY_RETENTION",
    "LSB_BHIST_IMPORT_CACHE_SIZE",
    "LSB_BHIST_DB_BUSY_TIMEOUT",
    "LSB_BHIST_QUERY_DB_CACHE_SIZE",
    "LSB_BHIST_QUERY_DB_MMAP_SIZE",
    "LSB_BHIST_MAX_FORMATTERS_PER_QUERY",
    "LSB_BHIST_JOBS_PER_BATCH",
    "LSB_BHIST_MAX_CONCURRENT_QUERIES",
    "LSB_BHIST_MAX_CONCURRENT_SCAN_QUERIES",
    "LSB_BHIST_MAX_QUEUED_QUERIES",
    "LSB_BHIST_CONNECT_TIMEOUT",
    "LSB_BHIST_RECEIVE_TIMEOUT"
};

/* Owns one parsed configuration value and its source line for diagnostics. */
struct configValue {
    char *value;
    int line;
};

/*
 * Format a configuration error when the caller supplied an output buffer.
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
 * Remove leading and trailing ASCII whitespace in place.
 * @param[in,out] value: Mutable NUL-terminated string.
 * @return: Pointer to the first non-whitespace character in value.
 */
static char *
trim(char *value)
{
    char *end;

    while (*value != '\0' && isspace((unsigned char)*value))
        value++;
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return value;
}

/*
 * Resolve a configuration key name to its configKey index.
 * @param[in] name: Key spelling from lsb.hist.
 * @return: Matching configKey index, or -1 for an unknown key.
 */
static int
findKey(const char *name)
{
    int i;

    for (i = 0; i < KEY_COUNT; i++) {
        if (strcmp(name, keyNames[i]) == 0)
            return i;
    }
    return -1;
}

/*
 * Release all strings owned by a parsed configuration value array.
 * @param[in,out] values: Complete KEY_COUNT-sized parsed value array.
 * @return: None.
 */
static void
freeValues(struct configValue values[KEY_COUNT])
{
    int i;

    for (i = 0; i < KEY_COUNT; i++) {
        free(values[i].value);
        values[i].value = NULL;
        values[i].line = 0;
    }
}

/*
 * Parse one lsb.hist file without shell evaluation or expansion.
 * @param[in] path: Absolute configuration file path.
 * @param[out] values: Parsed values indexed by configKey.
 * @param[out] error: Buffer receiving a parse or I/O error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
parseFile(const char *path, struct configValue values[KEY_COUNT],
           char *error, size_t errorSize)
{
    FILE *stream;
    char *line;
    size_t capacity;
    ssize_t length;
    int lineNumber;
    int result;

    stream = fopen(path, "r");
    if (stream == NULL) {
        setError(error, errorSize, "cannot open %s: %s", path,
                  strerror(errno));
        return -1;
    }
    line = NULL;
    capacity = 0;
    lineNumber = 0;
    result = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        char *equals;
        char *key;
        char *value;
        size_t valueLength;
        int index;

        lineNumber++;
        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';
        key = trim(line);
        if (key[0] == '\0' || key[0] == '#')
            continue;
        equals = strchr(key, '=');
        if (equals == NULL) {
            setError(error, errorSize, "%s:%d: expected KEY=VALUE",
                      path, lineNumber);
            result = -1;
            break;
        }
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);
        if (key[0] == '\0' || strchr(value, '=') == value) {
            setError(error, errorSize, "%s:%d: invalid assignment",
                      path, lineNumber);
            result = -1;
            break;
        }
        index = findKey(key);
        if (index < 0) {
            setError(error, errorSize, "%s:%d: unknown key %s",
                      path, lineNumber, key);
            result = -1;
            break;
        }
        if (values[index].value != NULL) {
            setError(error, errorSize,
                      "%s:%d: duplicate key %s (first set on line %d)",
                      path, lineNumber, key, values[index].line);
            result = -1;
            break;
        }
        valueLength = strlen(value);
        if (valueLength >= 2 &&
            ((value[0] == '"' && value[valueLength - 1] == '"') ||
             (value[0] == '\'' && value[valueLength - 1] == '\''))) {
            value[valueLength - 1] = '\0';
            value++;
        } else if ((valueLength > 0 &&
                    (value[0] == '"' || value[0] == '\'')) ||
                   (valueLength > 0 &&
                    (value[valueLength - 1] == '"' ||
                     value[valueLength - 1] == '\''))) {
            setError(error, errorSize, "%s:%d: unmatched quote",
                      path, lineNumber);
            result = -1;
            break;
        }
        values[index].value = strdup(value);
        if (values[index].value == NULL) {
            setError(error, errorSize, "%s:%d: out of memory",
                      path, lineNumber);
            result = -1;
            break;
        }
        values[index].line = lineNumber;
    }
    if (ferror(stream) && result == 0) {
        setError(error, errorSize, "cannot read %s: %s", path,
                  strerror(errno));
        result = -1;
    }
    free(line);
    fclose(stream);
    return result;
}

/*
 * Select a configured value or its default without transferring ownership.
 * @param[in] values: Parsed configuration value array.
 * @param[in] key: Key to inspect.
 * @param[in] defaultValue: Value used when the key was omitted.
 * @return: Configured value when present, otherwise defaultValue.
 */
static const char *
valueOrDefault(const struct configValue values[KEY_COUNT],
                 enum configKey key, const char *defaultValue)
{
    return values[key].value != NULL ? values[key].value : defaultValue;
}

/*
 * Copy a required configuration value into a fixed-size destination.
 * @param[out] destination: Destination buffer.
 * @param[in] destinationSize: Size of destination.
 * @param[in] name: Key name used in diagnostics.
 * @param[in] value: Required source value.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
copyValue(char *destination, size_t destinationSize, const char *name,
           const char *value, char *error, size_t errorSize)
{
    if (value == NULL || value[0] == '\0') {
        setError(error, errorSize, "%s is required", name);
        return -1;
    }
    if (snprintf(destination, destinationSize, "%s", value) >=
        (int)destinationSize) {
        setError(error, errorSize, "%s is too long", name);
        return -1;
    }
    return 0;
}

/*
 * Copy an optional configuration value into a fixed-size destination.
 * @param[out] destination: Destination buffer, cleared when value is NULL.
 * @param[in] destinationSize: Size of destination.
 * @param[in] name: Key name used in diagnostics.
 * @param[in] value: Optional source value.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
copyOptional(char *destination, size_t destinationSize, const char *name,
              const char *value, char *error, size_t errorSize)
{
    if (value == NULL) {
        destination[0] = '\0';
        return 0;
    }
    if (snprintf(destination, destinationSize, "%s", value) >=
        (int)destinationSize) {
        setError(error, errorSize, "%s is too long", name);
        return -1;
    }
    return 0;
}

/*
 * Parse and range-check one signed integer configuration value.
 * @param[in] name: Key name used in diagnostics.
 * @param[in] value: Decimal value text.
 * @param[in] minimum: Smallest accepted value.
 * @param[in] maximum: Largest accepted value.
 * @param[out] result: Parsed integer.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
parseInteger(const char *name, const char *value, long long minimum,
              long long maximum, long long *result,
              char *error, size_t errorSize)
{
    char *end;
    long long parsed;

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        setError(error, errorSize, "invalid %s=%s", name, value);
        return -1;
    }
    *result = parsed;
    return 0;
}

/*
 * Validate a service endpoint.
 * @param[in] endpoint: Endpoint in hostname:port form.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 for a valid endpoint, otherwise -1.
 */
static int
validateEndpoint(const char *endpoint,
                 char *error, size_t errorSize)
{
    struct in_addr ipv4;
    char host[254];
    const char *separator;
    const char *cursor;
    const char *label;
    char *end;
    long port;
    size_t hostLength;

    if (endpoint == NULL || endpoint[0] == '\0') {
        setError(error, errorSize, "LSB_BHIST_ENDPOINT is required");
        return -1;
    }
    separator = strrchr(endpoint, ':');
    if (separator == NULL || separator == endpoint || separator[1] == '\0' ||
        strchr(endpoint, '[') != NULL || strchr(endpoint, ']') != NULL ||
        memchr(endpoint, ':', (size_t)(separator - endpoint)) != NULL) {
        setError(error, errorSize,
                  "LSB_BHIST_ENDPOINT must be hostname:port: %s",
                  endpoint);
        return -1;
    }
    hostLength = (size_t)(separator - endpoint);
    if (hostLength > 253) {
        setError(error, errorSize,
                  "LSB_BHIST_ENDPOINT hostname is too long: %s", endpoint);
        return -1;
    }
    memcpy(host, endpoint, hostLength);
    host[hostLength] = '\0';
    if (inet_pton(AF_INET, host, &ipv4) == 1) {
        setError(error, errorSize,
                  "LSB_BHIST_ENDPOINT must use a hostname, not an IPv4 address: %s",
                  endpoint);
        return -1;
    }
    label = endpoint;
    for (cursor = endpoint; cursor <= separator; cursor++) {
        if (cursor == separator || *cursor == '.') {
            size_t labelLength;

            labelLength = (size_t)(cursor - label);
            if (labelLength == 0 || labelLength > 63 ||
                !isalnum((unsigned char)label[0]) ||
                !isalnum((unsigned char)label[labelLength - 1])) {
                setError(error, errorSize,
                          "LSB_BHIST_ENDPOINT has an invalid hostname: %s",
                          endpoint);
                return -1;
            }
            label = cursor + 1;
        } else if (!(isalnum((unsigned char)*cursor) || *cursor == '-')) {
            setError(error, errorSize,
                      "LSB_BHIST_ENDPOINT has an invalid hostname: %s",
                      endpoint);
            return -1;
        }
    }
    errno = 0;
    port = strtol(separator + 1, &end, 10);
    if (errno != 0 || end == separator + 1 || *end != '\0' ||
        port <= 0 || port > 65535) {
        setError(error, errorSize,
                  "LSB_BHIST_ENDPOINT has an invalid port: %s", endpoint);
        return -1;
    }
    return 0;
}

/*
 * Extract the host portion of a service endpoint.
 * @param[in] endpoint: Endpoint in hostname:port form.
 * @param[out] host: Buffer receiving the host name.
 * @param[in] hostSize: Size of host.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
extractEndpointHost(const char *endpoint, char *host, size_t hostSize,
                    char *error, size_t errorSize)
{
    const char *separator;
    size_t length;

    if (validateEndpoint(endpoint, error, errorSize) != 0)
        return -1;
    separator = strrchr(endpoint, ':');
    length = (size_t)(separator - endpoint);
    if (length >= hostSize) {
        setError(error, errorSize, "service hostname is too long");
        return -1;
    }
    memcpy(host, endpoint, length);
    host[length] = '\0';
    return 0;
}

/*
 * Verify that the current host matches the configured service host.
 * @param[in] config: Loaded configuration.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 for a match, otherwise -1.
 */
int
configRequireServiceHost(const struct config *config,
                                char *error, size_t errorSize)
{
    char serviceHost[256];
    char *localHost;

    if (config == NULL ||
        extractEndpointHost(config->endpoint,
                                 serviceHost, sizeof(serviceHost),
                                 error, errorSize) != 0)
        return -1;
    localHost = ls_getmyhostname();
    if (localHost == NULL || !equalHost_(serviceHost, localHost)) {
        setError(error, errorSize,
                  "this command must run on service host %s (local host is %s)",
                  serviceHost, localHost != NULL ? localHost : "unknown");
        return -1;
    }
    return 0;
}

/*
 * Join an absolute directory and one leaf name into a bounded path.
 * @param[in] directory: Absolute parent directory.
 * @param[in] leaf: Nonempty final path component.
 * @param[out] path: Buffer receiving the joined path.
 * @param[in] pathSize: Size of path.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
joinPath(const char *directory, const char *leaf,
          char *path, size_t pathSize,
          char *error, size_t errorSize)
{
    if (directory == NULL || directory[0] != '/' ||
        leaf == NULL || leaf[0] == '\0' ||
        snprintf(path, pathSize, "%s/%s", directory, leaf) >=
            (int)pathSize) {
        setError(error, errorSize, "configured path is invalid or too long");
        return -1;
    }
    return 0;
}

/*
 * Load role-specific Volclava paths and logging settings from lsf.conf.
 * @param[in] envdir: Directory containing lsf.conf.
 * @param[in] role: Configuration consumer role.
 * @param[in,out] config: Configuration receiving inherited LSF values.
 * @param[out] error: Buffer receiving an initialization error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
loadLsfEnvironment(const char *envdir, enum configRole role,
                     struct config *config,
                     char *error, size_t errorSize)
{
    struct config_param parameters[] = {
        { "LSF_BINDIR", NULL },
        { "LSF_LOGDIR", NULL },
        { "LSF_LOG_MASK", NULL },
        { "LSF_CONFDIR", NULL },
        { NULL, NULL }
    };
    int result;
    int i;

    result = initenv_(parameters, (char *)envdir);
    if (result < 0) {
        setError(error, errorSize, "cannot initialize %s/lsf.conf", envdir);
        goto done;
    }
    result = 0;
    if ((role & CONFIG_ROLE_SERVER) != 0) {
        if (copyValue(config->binDir, sizeof(config->binDir), "LSF_BINDIR",
                       parameters[0].paramValue, error, errorSize) != 0 ||
            config->binDir[0] != '/') {
            if (config->binDir[0] != '/')
                setError(error, errorSize,
                          "LSF_BINDIR must be an absolute path");
            result = -1;
            goto done;
        }
    }
    if ((role & (CONFIG_ROLE_SERVER | CONFIG_ROLE_LOADER)) != 0) {
        if (copyValue(config->confDir, sizeof(config->confDir),
                      "LSF_CONFDIR", parameters[3].paramValue,
                      error, errorSize) != 0)
            goto invalid;
        if (config->confDir[0] != '/') {
            setError(error, errorSize,
                     "LSF_CONFDIR must be an absolute path");
            goto invalid;
        }
        if (copyValue(config->logDir, sizeof(config->logDir),
                       "LSF_LOGDIR", parameters[1].paramValue,
                       error, errorSize) != 0 ||
            config->logDir[0] != '/') {
            if (config->logDir[0] != '/')
                setError(error, errorSize,
                          "LSF_LOGDIR must be an absolute path");
            result = -1;
            goto done;
        }
        if (copyOptional(config->logMask, sizeof(config->logMask),
                          "LSF_LOG_MASK", parameters[2].paramValue,
                          error, errorSize) != 0) {
            result = -1;
            goto done;
        }
        if (config->logMask[0] == '\0')
            snprintf(config->logMask, sizeof(config->logMask),
                     "LOG_WARNING");
    }
    if ((role & CONFIG_ROLE_SERVER) != 0 &&
        joinPath(config->binDir, "bhist_original",
                  config->bhistOriginalPath,
                  sizeof(config->bhistOriginalPath),
                  error, errorSize) != 0)
        result = -1;

done:
    for (i = 0; parameters[i].paramName != NULL; i++)
        free(parameters[i].paramValue);
    return result;

invalid:
    result = -1;
    goto done;
}

/* Owns names in first-occurrence order, for cluster and administrator lists. */
struct nameList {
    char **names;
    size_t count;
};

static void
freeNames(struct nameList *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->names[i]);
    free(list->names);
    list->names = NULL;
    list->count = 0;
}

static int
appendName(struct nameList *list, const char *name,
           char *error, size_t errorSize)
{
    char **names;
    char *copy;
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0)
            return 0;
    }
    if (list->count >= SIZE_MAX / sizeof(*names) - 1)
        goto noMemory;
    copy = strdup(name);
    if (copy == NULL)
        goto noMemory;
    names = realloc(list->names, (list->count + 1) * sizeof(*names));
    if (names == NULL) {
        free(copy);
        goto noMemory;
    }
    list->names = names;
    list->names[list->count++] = copy;
    return 0;

noMemory:
    setError(error, errorSize, "out of memory reading configuration names");
    return -1;
}

void
configFree(struct config *config)
{
    struct nameList admins;

    if (config == NULL)
        return;
    admins.names = config->admins;
    admins.count = config->adminCount;
    freeNames(&admins);
    free(config->clusterName);
    config->clusterName = NULL;
    config->admins = NULL;
    config->adminCount = 0;
}

/* Reuse liblsf comment/continuation handling, without its recursive blank skip. */
static int
readConfigLine(FILE *stream, const char *path, int *lineNumber, char **line,
               char *error, size_t errorSize)
{
    for (;;) {
        *line = getNextLineD_(stream, lineNumber, TRUE);
        if (ferror(stream) || lserrno != LSE_NO_ERR) {
            setError(error, errorSize, "%s:%d: cannot read configuration",
                     path, *lineNumber);
            return -1;
        }
        if (*line == NULL)
            return 0;
        /* getNextWord_ uses a fixed 4 * MAXLINELEN buffer. */
        if (strlen(*line) >= 4 * MAXLINELEN) {
            setError(error, errorSize, "%s:%d: configuration line is too long",
                     path, *lineNumber);
            return -1;
        }
        *line = trim(*line);
        if ((*line)[0] != '\0')
            return 1;
    }
}

/* A section body must terminate with its own End line. */
static int
readSectionLine(FILE *stream, const char *path, int *lineNumber,
                const char *section, char **line,
                char *error, size_t errorSize)
{
    char *cursor;
    char *word;
    int result = readConfigLine(stream, path, lineNumber, line,
                                error, errorSize);

    if (result < 0)
        return -1;
    if (result == 0) {
        setError(error, errorSize, "%s:%d: missing End %s",
                 path, *lineNumber, section);
        return -1;
    }
    cursor = *line;
    word = getNextWord_(&cursor);
    if (strcasecmp(word, "End") == 0) {
        word = getNextWord_(&cursor);
        if (word != NULL && strcasecmp(word, section) == 0 &&
            getNextWord_(&cursor) == NULL)
            return 0;
        setError(error, errorSize, "%s:%d: expected End %s",
                 path, *lineNumber, section);
        return -1;
    }
    if (strcasecmp(word, "Begin") == 0) {
        setError(error, errorSize, "%s:%d: nested section inside %s",
                 path, *lineNumber, section);
        return -1;
    }
    return 1;
}

static int
nextSection(FILE *stream, const char *path, int *lineNumber,
            char *section, size_t sectionSize,
            char *error, size_t errorSize)
{
    char *line;
    int result;

    while ((result = readConfigLine(stream, path, lineNumber, &line,
                                    error, errorSize)) > 0) {
        char *word = getNextWord_(&line);

        if (strcasecmp(word, "Begin") != 0)
            continue;
        word = getNextWord_(&line);
        if (word == NULL || strlen(word) >= sectionSize) {
            setError(error, errorSize, "%s:%d: invalid section name",
                     path, *lineNumber);
            return -1;
        }
        memcpy(section, word, strlen(word) + 1);
        if (getNextWord_(&line) != NULL) {
            setError(error, errorSize, "%s:%d: invalid Begin %s",
                     path, *lineNumber, section);
            return -1;
        }
        return 1;
    }
    return result;
}

/* Split one table field in place, including the usual parenthesized list. */
static int
nextTableValue(char **cursor, char **value)
{
    char *end;

    *cursor = trim(*cursor);
    if (**cursor == '\0')
        return 0;
    *value = *cursor;
    if (**cursor == '(') {
        *value = ++*cursor;
        end = strchr(*cursor, ')');
        if (end == NULL || (end[1] != '\0' && !isspace((unsigned char)end[1])))
            return -1;
        *end = '\0';
        *cursor = end + 1;
        *value = trim(*value);
        return 1;
    }
    end = *cursor;
    while (*end != '\0' && !isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        *end++ = '\0';
    *cursor = end;
    return 1;
}

static int
readClusterSection(FILE *stream, const char *path, int *lineNumber,
                   struct nameList *clusters, char *error, size_t errorSize)
{
    char *line;
    char *word;
    int columns = 0;
    int clusterColumn = -1;
    int result;

    result = readSectionLine(stream, path, lineNumber, "Cluster", &line,
                             error, errorSize);
    if (result < 0)
        return -1;
    if (result == 0)
        goto invalid;
    while ((word = getNextWord_(&line)) != NULL) {
        if (strcasecmp(word, "ClusterName") == 0) {
            if (clusterColumn >= 0)
                goto invalid;
            clusterColumn = columns;
        }
        columns++;
    }
    if (clusterColumn < 0)
        goto invalid;
    while ((result = readSectionLine(stream, path, lineNumber, "Cluster", &line,
                                     error, errorSize)) > 0) {
        char *name = NULL;
        int column = 0;
        int valueResult;

        while ((valueResult = nextTableValue(&line, &word)) > 0) {
            if (column == clusterColumn)
                name = word;
            column++;
        }
        if (valueResult < 0 || column != columns || name == NULL ||
            name[0] == '\0' || strcmp(name, "-") == 0 ||
            strpbrk(name, "/\\ \t\r\n") != NULL)
            goto invalid;
        if (appendName(clusters, name, error, errorSize) != 0)
            return -1;
    }
    return result;

invalid:
    setError(error, errorSize, "%s:%d: invalid Cluster table (requires ClusterName)",
             path, *lineNumber);
    return -1;
}

static int
readClusterNames(const char *path, struct nameList *clusters,
                 char *error, size_t errorSize)
{
    FILE *stream = fopen(path, "r");
    char section[64];
    int lineNumber = 0;
    int result;

    if (stream == NULL) {
        setError(error, errorSize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    while ((result = nextSection(stream, path, &lineNumber,
                                  section, sizeof(section), error, errorSize)) > 0) {
        char *line;

        if (strcasecmp(section, "Cluster") == 0)
            result = readClusterSection(stream, path, &lineNumber, clusters,
                                         error, errorSize);
        else {
            while ((result = readSectionLine(stream, path, &lineNumber,
                                             section, &line, error, errorSize)) > 0)
                ;
        }
        if (result < 0)
            break;
    }
    fclose(stream);
    if (result == 0 && clusters->count == 0) {
        setError(error, errorSize, "%s: no cluster names in Cluster sections", path);
        result = -1;
    }
    return result;
}

/* Retain one definition; modern and legacy sections are selected after parsing. */
static int
readAdminSection(FILE *stream, const char *path, int *lineNumber,
                 const char *section, const char *key, char **definition,
                 char *error, size_t errorSize)
{
    char *line;
    char *value;
    char *equals;
    char *cursor;
    int result;

    if (*definition != NULL) {
        setError(error, errorSize, "%s:%d: duplicate %s section",
                 path, *lineNumber, section);
        return -1;
    }
    result = readSectionLine(stream, path, lineNumber, section, &line,
                             error, errorSize);
    if (result < 0)
        return -1;
    if (result == 0)
        goto invalid;
    equals = strchr(line, '=');
    if (equals != NULL) {
        *equals = '\0';
        if (strcasecmp(trim(line), key) != 0)
            goto invalid;
        value = trim(equals + 1);
        if (value[0] == '(') {
            cursor = value;
            if (nextTableValue(&cursor, &value) != 1 || *trim(cursor) != '\0')
                goto invalid;
        }
    } else {
        if (strcasecmp(trim(line), key) != 0)
            goto invalid;
        result = readSectionLine(stream, path, lineNumber, section, &line,
                                 error, errorSize);
        if (result < 0)
            return -1;
        if (result == 0 || nextTableValue(&line, &value) != 1 ||
            *trim(line) != '\0')
            goto invalid;
    }
    if (value[0] == '\0' || strcmp(value, "-") == 0)
        goto invalid;
    *definition = strdup(value);
    if (*definition == NULL) {
        setError(error, errorSize, "%s:%d: out of memory", path, *lineNumber);
        return -1;
    }
    result = readSectionLine(stream, path, lineNumber, section, &line,
                             error, errorSize);
    if (result <= 0)
        return result;

invalid:
    setError(error, errorSize, "%s:%d: expected one nonempty %s definition in %s",
             path, *lineNumber, key, section);
    return -1;
}

static int
expandAdmins(char *definition, struct nameList *admins,
             char *error, size_t errorSize)
{
    char *word;

    while ((word = getNextWord_(&definition)) != NULL) {
        struct group *group;
        char **member;

        if (getpwnam(word) != NULL) {
            if (appendName(admins, word, error, errorSize) != 0)
                return -1;
            continue;
        }
        group = getgrnam(word);
        if (group == NULL) {
            setError(error, errorSize, "administrator %s is not a user or Unix group",
                     word);
            return -1;
        }
        for (member = group->gr_mem; member != NULL && *member != NULL; member++) {
            if (getpwnam(*member) == NULL) {
                setError(error, errorSize, "administrator group member %s is not a user",
                         *member);
                return -1;
            }
            if (appendName(admins, *member, error, errorSize) != 0)
                return -1;
        }
    }
    if (admins->count == 0) {
        setError(error, errorSize, "administrator list contains no users");
        return -1;
    }
    return 0;
}

static int
readAdmins(const char *path, struct nameList *admins,
           char *error, size_t errorSize)
{
    FILE *stream = fopen(path, "r");
    char section[64];
    char *modern = NULL;
    char *legacy = NULL;
    int lineNumber = 0;
    int result;

    if (stream == NULL) {
        setError(error, errorSize, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    while ((result = nextSection(stream, path, &lineNumber,
                                  section, sizeof(section), error, errorSize)) > 0) {
        char *line;

        if (strcasecmp(section, "ClusterAdmins") == 0)
            result = readAdminSection(stream, path, &lineNumber, section,
                                       "Administrators", &modern, error, errorSize);
        else if (strcasecmp(section, "ClusterManager") == 0)
            result = readAdminSection(stream, path, &lineNumber, section,
                                       "Manager", &legacy, error, errorSize);
        else {
            while ((result = readSectionLine(stream, path, &lineNumber, section,
                                             &line, error, errorSize)) > 0)
                ;
        }
        if (result < 0)
            break;
    }
    fclose(stream);
    if (result == 0) {
        if (modern == NULL && legacy == NULL) {
            setError(error, errorSize, "%s: missing ClusterAdmins/ClusterManager", path);
            result = -1;
        } else
            result = expandAdmins(modern != NULL ? modern : legacy,
                                    admins, error, errorSize);
    }
    free(modern);
    free(legacy);
    return result;
}

int
configLoadAdmins(struct config *config, char *error, size_t errorSize)
{
    struct nameList clusters = { NULL, 0 };
    struct nameList admins = { NULL, 0 };
    char path[CONFIG_PATH_MAX];
    char selectedPath[CONFIG_PATH_MAX];
    char *selectedName = NULL;
    size_t i;
    int result = -1;

    if (config == NULL) {
        setError(error, errorSize, "configuration is required");
        return -1;
    }
    configFree(config);
    if (joinPath(config->confDir, "lsf.shared", path, sizeof(path),
                  error, errorSize) != 0 ||
        readClusterNames(path, &clusters, error, errorSize) != 0)
        goto done;
    for (i = 0; i < clusters.count; i++) {
        struct stat status;

        if (snprintf(path, sizeof(path), "%s/lsf.cluster.%s",
                     config->confDir, clusters.names[i]) >= (int)sizeof(path)) {
            setError(error, errorSize, "cluster configuration path is too long");
            goto done;
        }
        if (stat(path, &status) != 0) {
            if (errno == ENOENT) {
                if (lstat(path, &status) != 0 && errno == ENOENT)
                    continue;
                setError(error, errorSize, "cannot resolve cluster configuration %s",
                         path);
                goto done;
            }
            setError(error, errorSize, "cannot inspect %s: %s", path, strerror(errno));
            goto done;
        }
        if (!S_ISREG(status.st_mode)) {
            setError(error, errorSize, "%s is not a regular file", path);
            goto done;
        }
        if (selectedName != NULL) {
            setError(error, errorSize,
                     "multiple cluster configuration files exist: %s and %s",
                     selectedName, clusters.names[i]);
            goto done;
        }
        selectedName = clusters.names[i];
        memcpy(selectedPath, path, strlen(path) + 1);
    }
    if (selectedName == NULL) {
        setError(error, errorSize, "no matching lsf.cluster.<name> exists in %s",
                 config->confDir);
        goto done;
    }
    if (readAdmins(selectedPath, &admins, error, errorSize) != 0)
        goto done;
    config->clusterName = strdup(selectedName);
    if (config->clusterName == NULL) {
        setError(error, errorSize, "out of memory copying cluster name");
        goto done;
    }
    config->admins = admins.names;
    config->adminCount = admins.count;
    admins.names = NULL;
    admins.count = 0;
    result = 0;

done:
    freeNames(&clusters);
    freeNames(&admins);
    return result;
}

/*
 * Apply defaults and validate only the fields consumed by the selected role.
 * @param[in] values: Parsed lsb.hist values.
 * @param[in] role: Configuration consumer role.
 * @param[in,out] config: Configuration receiving validated values.
 * @param[out] error: Buffer receiving a validation error.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
validateAndAssign(const struct configValue values[KEY_COUNT],
                    enum configRole role, struct config *config,
                    char *error, size_t errorSize)
{
    const char *value;
    long long parsed;

    if ((role & (CONFIG_ROLE_CLIENT | CONFIG_ROLE_SERVER |
                 CONFIG_ROLE_LOADER)) != 0) {
        value = values[KEY_SERVICE_ENDPOINT].value;
        if (validateEndpoint(value, error, errorSize) != 0 ||
            copyValue(config->endpoint,
                       sizeof(config->endpoint),
                       "LSB_BHIST_ENDPOINT", value,
                       error, errorSize) != 0)
            return -1;
        value = valueOrDefault(values,
                                 KEY_CONNECT_TIMEOUT, "5");
        if (parseInteger("LSB_BHIST_CONNECT_TIMEOUT", value,
                          1, 3600, &parsed, error, errorSize) != 0)
            return -1;
        config->connectTimeoutSeconds = (int)parsed;
    }
    if ((role & (CONFIG_ROLE_SERVER | CONFIG_ROLE_LOADER)) != 0) {
        value = values[KEY_DB_PATH].value;
        if (copyValue(config->dbPath,
                       sizeof(config->dbPath),
                       "LSB_BHIST_DB_PATH", value,
                       error, errorSize) != 0)
            return -1;
        if (config->dbPath[0] != '/') {
            setError(error, errorSize,
                      "LSB_BHIST_DB_PATH must be an absolute local path: %s",
                      config->dbPath);
            return -1;
        }
        value = valueOrDefault(values, KEY_DB_BUSY_TIMEOUT, "60000");
        if (parseInteger("LSB_BHIST_DB_BUSY_TIMEOUT", value, 0,
                          INT_MAX, &parsed, error, errorSize) != 0)
            return -1;
        config->dbBusyTimeoutMs = (int)parsed;
    }
    if ((role & CONFIG_ROLE_LOADER) != 0) {
        if (copyOptional(config->eventDir,
                          sizeof(config->eventDir),
                          "LSB_BHIST_EVENT_DIR",
                          values[KEY_EVENT_DIR].value,
                          error, errorSize) != 0)
            return -1;
        if (config->eventDir[0] != '\0' &&
            config->eventDir[0] != '/') {
            setError(error, errorSize,
                      "LSB_BHIST_EVENT_DIR must be an absolute path: %s",
                      config->eventDir);
            return -1;
        }
#define ASSIGN_LOADER_INT(KEY, FIELD, DEFAULT_VALUE, MINIMUM, MAXIMUM) \
        value = valueOrDefault(values, KEY, DEFAULT_VALUE); \
        if (parseInteger(keyNames[KEY], value, MINIMUM, MAXIMUM, &parsed, \
                          error, errorSize) != 0) \
            return -1; \
        config->FIELD = (int)parsed

        ASSIGN_LOADER_INT(KEY_EVENT_POLL_INTERVAL,
                          eventPollIntervalMs, "5000", 1, INT_MAX);
        value = valueOrDefault(values, KEY_MAX_EVENTS_PER_TRANSACTION,
                               "100000");
        if (parseInteger(keyNames[KEY_MAX_EVENTS_PER_TRANSACTION], value, 1,
                          LLONG_MAX - 1, &config->maxEventsPerTransaction,
                          error, errorSize) != 0)
            return -1;
        value = valueOrDefault(values,
                               KEY_MAX_RAW_SIZE_PER_TRANSACTION, "64");
        if (parseInteger(keyNames[KEY_MAX_RAW_SIZE_PER_TRANSACTION],
                          value, 1,
                          LLONG_MAX / (1024LL * 1024LL),
                          &config->maxRawSizePerTransactionMib,
                          error, errorSize) != 0)
            return -1;
        value = valueOrDefault(values, KEY_HISTORY_RETENTION,
                                 "2592000");
        if (parseInteger(keyNames[KEY_HISTORY_RETENTION], value, 1,
                          LLONG_MAX - 1, &config->historyRetentionSeconds,
                          error, errorSize) != 0)
            return -1;
        ASSIGN_LOADER_INT(KEY_IMPORT_CACHE_SIZE,
                          importCacheSizeMib, "1024", 1,
                          INT_MAX / 1024);
#undef ASSIGN_LOADER_INT
    }
    if ((role & CONFIG_ROLE_SERVER) != 0) {
#define ASSIGN_SERVER_INT(KEY, FIELD, DEFAULT_VALUE, MINIMUM, MAXIMUM) \
        value = valueOrDefault(values, KEY, DEFAULT_VALUE); \
        if (parseInteger(keyNames[KEY], value, MINIMUM, MAXIMUM, &parsed, \
                          error, errorSize) != 0) \
            return -1; \
        config->FIELD = (int)parsed

        ASSIGN_SERVER_INT(KEY_QUERY_DB_CACHE_SIZE,
                          queryDbCacheSizeMib, "256", 1,
                          INT_MAX / 1024);
        ASSIGN_SERVER_INT(KEY_QUERY_DB_MMAP_SIZE,
                          queryDbMmapSizeMib, "256", 0,
                          INT_MAX);
        ASSIGN_SERVER_INT(KEY_MAX_FORMATTERS_PER_QUERY,
                          formattersPerQuery, "4", 1, 128);
        ASSIGN_SERVER_INT(KEY_JOBS_PER_BATCH, jobsPerBatch, "10000", 1,
                          INT_MAX);
        ASSIGN_SERVER_INT(KEY_MAX_CONCURRENT_QUERIES, maxConcurrentQueries,
                          "16", 1, 128);
        ASSIGN_SERVER_INT(KEY_MAX_CONCURRENT_SCAN_QUERIES,
                          maxConcurrentScanQueries,
                          "4", 1, 128);
        if (config->maxConcurrentScanQueries >
            config->maxConcurrentQueries) {
            setError(error, errorSize,
                      "LSB_BHIST_MAX_CONCURRENT_SCAN_QUERIES must not exceed "
                      "LSB_BHIST_MAX_CONCURRENT_QUERIES");
            return -1;
        }
        ASSIGN_SERVER_INT(KEY_MAX_QUEUED_QUERIES, maxQueuedQueries,
                          "128", 0, 4096);
        ASSIGN_SERVER_INT(KEY_RECEIVE_TIMEOUT,
                          receiveTimeoutSeconds, "5", 1, 3600);
#undef ASSIGN_SERVER_INT
    }
    return 0;
}

/*
 * Load configuration from explicit paths.
 * @param[in] envdir: Directory containing lsf.conf.
 * @param[in] configPath: Path to lsb.hist.
 * @param[in] role: Configuration consumer role.
 * @param[out] config: Parsed configuration.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
loadFromFile(const char *envdir, const char *configPath,
             enum configRole role, struct config *config,
             char *error, size_t errorSize)
{
    struct configValue values[KEY_COUNT];
    int result;

    if (envdir == NULL || envdir[0] != '/' ||
        configPath == NULL || configPath[0] != '/' || config == NULL) {
        setError(error, errorSize,
                  "LSF_ENVDIR and lsb.hist path must be absolute");
        return -1;
    }
    memset(config, 0, sizeof(*config));
    memset(values, 0, sizeof(values));
    if (snprintf(config->envDir, sizeof(config->envDir), "%s", envdir) >=
            (int)sizeof(config->envDir) ||
        snprintf(config->configPath, sizeof(config->configPath), "%s",
                 configPath) >= (int)sizeof(config->configPath)) {
        setError(error, errorSize, "configuration path is too long");
        return -1;
    }
    if (loadLsfEnvironment(envdir, role, config,
                             error, errorSize) != 0)
        return -1;
    if (parseFile(configPath, values, error, errorSize) != 0) {
        freeValues(values);
        return -1;
    }
    result = validateAndAssign(values, role, config,
                                 error, errorSize);
    freeValues(values);
    return result;
}

/*
 * Load lsb.hist and lsf.conf for a consumer role.
 * @param[in] role: Configuration consumer role.
 * @param[out] config: Parsed configuration.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
configLoad(enum configRole role, struct config *config,
                char *error, size_t errorSize)
{
    const char *envdir;
    char path[CONFIG_PATH_MAX];

    if (config == NULL) {
        setError(error, errorSize, "configuration is required");
        return -1;
    }
    memset(config, 0, sizeof(*config));
    envdir = getenv("LSF_ENVDIR");
    if (envdir == NULL || envdir[0] != '/') {
        setError(error, errorSize,
                  "LSF_ENVDIR must be set to an absolute configuration directory");
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/lsb.hist", envdir) >=
        (int)sizeof(path)) {
        setError(error, errorSize,
                 "lsb.hist path is too long");
        return -1;
    }
    return loadFromFile(envdir, path, role, config,
                        error, errorSize);
}

/*
 * Build the runtime log path for a component.
 * @param[in] config: Loaded configuration.
 * @param[in] name: Component name.
 * @param[out] path: Buffer receiving the log path.
 * @param[in] pathSize: Size of path.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
configLogPath(const struct config *config, const char *name,
                    char *path, size_t pathSize,
                    char *error, size_t errorSize)
{
    char *hostname;

    if (config == NULL || name == NULL || name[0] == '\0' ||
        config->logDir[0] == '\0') {
        setError(error, errorSize,
                  "LSF_LOGDIR is not configured for file logging");
        return -1;
    }
    hostname = ls_getmyhostname();
    if (hostname == NULL || hostname[0] == '\0')
        hostname = "unknown";
    if (snprintf(path, pathSize, "%s/%s.log.%s",
                 config->logDir, name, hostname) >= (int)pathSize) {
        setError(error, errorSize, "runtime log path is too long");
        return -1;
    }
    return 0;
}

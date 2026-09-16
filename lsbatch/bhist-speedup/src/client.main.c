/*
 * Public bhist-speedup client.
 *
 * The -h and -V options complete locally before configuration is loaded, so
 * help and version output remain available when the service is down. Each
 * query loads the client configuration again before opening a one-shot TCP
 * connection. Arguments are encoded without rewriting and parsed by the
 * server-side query backend. The local user name is only a default filter
 * hint when the command does not contain an explicit -u option.
 *
 * STDOUT and STDERR frames are streamed to their corresponding local file
 * descriptors in arrival order. The EXIT frame determines the client exit
 * status. If the connection fails after partial output, the client must report
 * an incomplete result rather than let automation treat truncated output as
 * success.
 */

#include "bhist.config.h"
#include "protocol.h"
#include "network.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLIENT_FALLBACK_SAFE_STATUS 125
#define CLIENT_PARTIAL_OUTPUT_STATUS 126

/*
 * Print the public client command syntax.
 * @return: None.
 */
static void
usage(void)
{
    fputs("Usage: bhist-speedup [-l | -w] [-a] [-d] [-e] [-p] [-s] [-r]\n"
          "                 [-C time0,time1] [-S time0,time1] "
          "[-D time0,time1]\n"
          "                 [-q queue_name] [-m host_name]\n"
          "                 [-u user_name | -u all] "
          "[jobId | \"jobId[indexList]\"]\n"
          "       bhist-speedup [-h] [-V]\n", stdout);
}

/*
 * Read a 32-bit unsigned integer in network byte order.
 * @param[in] payload: Buffer containing at least four bytes.
 * @return: The value converted to host byte order.
 */
static uint32_t
readU32(const unsigned char *payload)
{
    uint32_t value;

    memcpy(&value, payload, sizeof(value));
    return ntohl(value);
}

/*
 * Stream query output and translate the server-side exit status.
 * @param[in] fd: Server connection on which the query was submitted.
 * @return: Query exit code, CLIENT_FALLBACK_SAFE_STATUS before stdout, or
 *          CLIENT_PARTIAL_OUTPUT_STATUS after stdout.
 */
static int
receiveQueryResponse(int fd)
{
    unsigned char *payload;
    uint32_t payloadLength;
    uint32_t exitCode;
    uint32_t signalNumber;
    uint8_t type;
    int sawStdout;
    int rc;

    sawStdout = 0;
    for (;;) {
        payload = NULL;
        payloadLength = 0;
        rc = receiveFrame(fd, &type, &payload, &payloadLength,
                            PROTOCOL_DATA_MAX);
        if (rc != 0) {
            if (sawStdout)
                fprintf(stderr,
                        "bhist speed up: service connection lost after partial "
                        "output; results are incomplete\n");
            else
                fprintf(stderr, "bhist speed up: service connection lost\n");
            free(payload);
            return sawStdout ? CLIENT_PARTIAL_OUTPUT_STATUS :
                CLIENT_FALLBACK_SAFE_STATUS;
        }
        if (type == FRAME_STDOUT || type == FRAME_STDERR) {
            int outputFd;

            outputFd = type == FRAME_STDOUT ? STDOUT_FILENO :
                STDERR_FILENO;
            if (payloadLength > 0 &&
                writeAllFd(outputFd, payload, payloadLength) != 0) {
                free(payload);
                return CLIENT_PARTIAL_OUTPUT_STATUS;
            }
            if (type == FRAME_STDOUT && payloadLength > 0)
                sawStdout = 1;
        } else if (type == FRAME_EXIT && payloadLength == 8) {
            int queryStatus;

            exitCode = readU32(payload);
            signalNumber = readU32(payload + 4);
            free(payload);
            if (signalNumber != 0)
                queryStatus = signalNumber > 127 ? 255 :
                    128 + (int)signalNumber;
            else
                queryStatus = exitCode > 255 ? 255 : (int)exitCode;
            if (queryStatus != 0)
                return sawStdout ? CLIENT_PARTIAL_OUTPUT_STATUS :
                    CLIENT_FALLBACK_SAFE_STATUS;
            return 0;
        } else if (type == FRAME_ERROR && payloadLength >= 4) {
            fprintf(stderr, "bhist speed up: service error: %.*s\n",
                    (int)(payloadLength - 4), (char *)(payload + 4));
            free(payload);
            return sawStdout ? CLIENT_PARTIAL_OUTPUT_STATUS :
                CLIENT_FALLBACK_SAFE_STATUS;
        } else {
            fprintf(stderr, "bhist speed up: invalid service response frame\n");
            free(payload);
            return sawStdout ? CLIENT_PARTIAL_OUTPUT_STATUS :
                CLIENT_FALLBACK_SAFE_STATUS;
        }
        free(payload);
    }
}

/*
 * Parse the client command and handle local options or submit a query.
 * @param[in] argc: Number of command-line arguments.
 * @param[in] argv: Command-line argument vector.
 * @return: 0 on success, 2 for invalid arguments, or another nonzero value
 *          on failure.
 */
int
main(int argc, char **argv)
{
    struct config config = {0};
    struct queryRequest request;
    unsigned char *requestPayload;
    struct passwd *passwordEntry;
    char userHint[PROTOCOL_USER_HINT_MAX + 1];
    char error[512];
    uint32_t requestLength;
    int fd;
    int rc;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage();
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "-V") == 0 ||
                      strcmp(argv[1], "--version") == 0)) {
        fputs(PROGRAM_VERSION, stdout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "perfmon") == 0) {
        fprintf(stderr,
                "bhist speed up: perfmon is a server command; run "
                "bhist-speedup-server perfmon {seconds|stop} on the service "
                "host\n");
        return 2;
    }
    if (argc >= 2 && strcmp(argv[1], "view") == 0) {
        fprintf(stderr,
                "bhist speed up: view is a server command; run "
                "bhist-speedup-server view on the service host\n");
        return 2;
    }
    if (argc - 1 > PROTOCOL_ARGC_MAX) {
        fprintf(stderr, "bhist speed up: too many arguments\n");
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    if (configLoad(CONFIG_ROLE_CLIENT, &config,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist speed up: %s\n", error);
        configFree(&config);
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    passwordEntry = getpwuid(getuid());
    if (passwordEntry == NULL || passwordEntry->pw_name == NULL ||
        passwordEntry->pw_name[0] == '\0' ||
        strlen(passwordEntry->pw_name) > PROTOCOL_USER_HINT_MAX) {
        fprintf(stderr, "bhist speed up: cannot resolve the local user\n");
        configFree(&config);
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    snprintf(userHint, sizeof(userHint), "%s", passwordEntry->pw_name);

    memset(&request, 0, sizeof(request));
    request.userHint = userHint;
    request.argc = argc - 1;
    request.argv = argv + 1;
    requestPayload = NULL;
    requestLength = 0;
    if (encodeQueryRequest(&request, &requestPayload,
                                 &requestLength) != 0) {
        fprintf(stderr, "bhist speed up: request arguments are too large\n");
        configFree(&config);
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    fd = connectEndpoint(config.endpoint,
                              config.connectTimeoutSeconds);
    if (fd < 0) {
        fprintf(stderr, "bhist speed up: cannot connect to service %s: %s\n",
                config.endpoint, strerror(errno));
        free(requestPayload);
        configFree(&config);
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    rc = sendFrame(fd, FRAME_QUERY, requestPayload,
                        requestLength);
    free(requestPayload);
    if (rc != 0) {
        fprintf(stderr, "bhist speed up: cannot send query request: %s\n",
                strerror(errno));
        close(fd);
        configFree(&config);
        return CLIENT_FALLBACK_SAFE_STATUS;
    }
    rc = receiveQueryResponse(fd);
    close(fd);
    configFree(&config);
    return rc;
}

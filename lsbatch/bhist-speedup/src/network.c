#include "network.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * TCP connection setup and file-descriptor output helpers. This module
 * performs no protocol framing or command authorization.
 */
static int parseEndpoint(const char *endpoint, char *host, size_t hostLen);
static int connectAddress(const struct addrinfo *address,
                          int timeoutSeconds);

/*
 * Validate an endpoint and extract its port and optional host.
 * @param[in] endpoint: IPv4, IPv6, or hostname endpoint text.
 * @param[out] host: Buffer receiving the host portion, or NULL if not needed.
 * @param[in] hostLen: Size of host; ignored when host is NULL.
 * @return: Port number in the range 1-65535, otherwise -1.
 */
static int
parseEndpoint(const char *endpoint, char *host, size_t hostLen)
{
    const char *hostStart;
    const char *hostEnd;
    const char *portStart;
    const char *separator;
    size_t length;
    char *end;
    long parsedPort;

    if (endpoint == NULL || endpoint[0] == '\0')
        return -1;

    if (endpoint[0] == '[') {
        hostStart = endpoint + 1;
        hostEnd = strchr(hostStart, ']');
        if (hostEnd == NULL || hostEnd[1] != ':' || hostEnd[2] == '\0')
            return -1;
        portStart = hostEnd + 2;
    } else {
        separator = strrchr(endpoint, ':');
        if (separator == NULL || separator == endpoint ||
            separator[1] == '\0')
            return -1;
        hostStart = endpoint;
        hostEnd = separator;
        portStart = separator + 1;
        if (memchr(hostStart, ':', (size_t)(hostEnd - hostStart)) != NULL)
            return -1;
    }

    length = (size_t)(hostEnd - hostStart);
    if (length == 0 || length >= NETWORK_ENDPOINT_MAX ||
        (host != NULL && length >= hostLen))
        return -1;

    errno = 0;
    parsedPort = strtol(portStart, &end, 10);
    if (errno != 0 || end == portStart || *end != '\0' || parsedPort <= 0 ||
        parsedPort > 65535)
        return -1;
    if (host != NULL) {
        memcpy(host, hostStart, length);
        host[length] = '\0';
    }
    return (int)parsedPort;
}
/*
 * Apply common options to a connected socket.
 * @param[in] fd: Connected socket descriptor.
 * @return: None.
 */
void
configureConnectedSocket(int fd)
{
    int enabled;

    enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled,
                     sizeof(enabled));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     sizeof(enabled));
}

/*
 * Connect to one resolved address with a bounded nonblocking wait.
 * @param[in] address: Resolved socket address candidate.
 * @param[in] timeoutSeconds: Connect timeout in seconds.
 * @return: Connected descriptor on success, otherwise -1.
 */
static int
connectAddress(const struct addrinfo *address, int timeoutSeconds)
{
    struct pollfd pollFd;
    socklen_t errorLen;
    int fd;
    int flags;
    int socketError;
    int rc;

    fd = socket(address->ai_family, address->ai_socktype,
                address->ai_protocol);
    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }

    rc = connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        memset(&pollFd, 0, sizeof(pollFd));
        pollFd.fd = fd;
        pollFd.events = POLLOUT;
        do {
            rc = poll(&pollFd, 1, timeoutSeconds * 1000);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0) {
            if (rc == 0)
                errno = ETIMEDOUT;
            close(fd);
            return -1;
        }
        socketError = 0;
        errorLen = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                       &errorLen) != 0 || socketError != 0) {
            if (socketError != 0)
                errno = socketError;
            close(fd);
            return -1;
        }
    }

    if (fcntl(fd, F_SETFL, flags) != 0) {
        close(fd);
        return -1;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    configureConnectedSocket(fd);
    return fd;
}

/*
 * Connect to a service endpoint within a timeout.
 * @param[in] endpoint: Endpoint in hostname:port or IPv4:port form.
 * @param[in] timeoutSeconds: Connection timeout in seconds.
 * @return: Connected socket descriptor, otherwise -1.
 */
int
connectEndpoint(const char *endpoint, int timeoutSeconds)
{
    struct addrinfo hints;
    struct addrinfo *addresses;
    struct addrinfo *address;
    char host[NETWORK_ENDPOINT_MAX];
    char port[16];
    int fd;
    int portNumber;
    int savedErrno;

    if (timeoutSeconds <= 0)
        timeoutSeconds = 5;
    portNumber = parseEndpoint(endpoint, host, sizeof(host));
    if (portNumber < 0) {
        errno = EINVAL;
        return -1;
    }
    snprintf(port, sizeof(port), "%d", portNumber);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &addresses) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    fd = -1;
    savedErrno = ECONNREFUSED;
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = connectAddress(address, timeoutSeconds);
        if (fd >= 0)
            break;
        savedErrno = errno;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        errno = savedErrno;
    return fd;
}

/*
 * Listen on all local IPv4 addresses using the service endpoint's port.
 * @param[in] endpoint: Endpoint in hostname:port or IPv4:port form.
 * @param[in] backlog: Listen queue length.
 * @return: Listener descriptor, otherwise -1.
 */
int
listenEndpoint(const char *endpoint, int backlog)
{
    struct sockaddr_in address;
    int enabled;
    int fd;
    int port;
    int savedErrno;

    port = parseEndpoint(endpoint, NULL, 0);
    if (port < 0) {
        errno = EINVAL;
        return -1;
    }
    if (backlog <= 0)
        backlog = 16;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, backlog) < 0) {
        savedErrno = errno;
        close(fd);
        errno = savedErrno;
        return -1;
    }
    return fd;
}

/*
 * Set a socket receive timeout.
 * @param[in] fd: Socket descriptor.
 * @param[in] timeoutSeconds: Timeout in seconds.
 * @return: 0 on success, otherwise -1.
 */
int
setReceiveTimeout(int fd, int timeoutSeconds)
{
    struct timeval timeout;

    if (timeoutSeconds <= 0)
        timeoutSeconds = 5;
    timeout.tv_sec = timeoutSeconds;
    timeout.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout));
}
/*
 * Write a complete buffer to a file descriptor.
 * @param[in] fd: Destination file descriptor.
 * @param[in] data: Data to write.
 * @param[in] length: Number of bytes to write.
 * @return: 0 on success, otherwise -1.
 */
int
writeAllFd(int fd, const void *data, size_t length)
{
    const unsigned char *cursor;
    ssize_t written;

    cursor = (const unsigned char *)data;
    while (length > 0) {
        written = write(fd, cursor, length);
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

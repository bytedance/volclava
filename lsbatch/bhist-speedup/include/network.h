#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>

#define NETWORK_ENDPOINT_MAX 512

int connectEndpoint(const char *endpoint, int timeoutSeconds);

int listenEndpoint(const char *endpoint, int backlog);

void configureConnectedSocket(int fd);

int setReceiveTimeout(int fd, int timeoutSeconds);

int writeAllFd(int fd, const void *data, size_t length);

#endif

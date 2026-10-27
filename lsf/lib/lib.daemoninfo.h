/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */

#ifndef LIB_DAEMONINFO_H
#define LIB_DAEMONINFO_H

#include <time.h>
#include "lib.hdr.h"

struct config_param;

struct showConfReply {
    time_t configTime;
    int entryCount;
    struct config_param *entries;
};

#define SHOWCONF_MBD 0x1
#define SHOWCONF_SBD 0x2
#define SHOWCONF_LIM 0x4

extern int xdrShowConfReplySize(const struct showConfReply *);
extern bool_t xdr_showConfReply(XDR *, struct showConfReply *,
                                struct LSFHeader *);
extern int initShowconfParams(struct config_param *);
extern int makeShowConfReply(int, struct showConfReply *);
extern void freeShowConfReply(struct showConfReply *, int);
extern void printShowConfReply(const char *, const char *,
                               const struct showConfReply *);

#endif

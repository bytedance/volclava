/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */
#include "lib.h"
#include "lib.daemoninfo.h"

struct showconf_param {
    struct config_param paramInfo;
    int daemonMask;
    int needFree;
};

/**
 * Static showconf catalog and fallback values.
 * initShowconfParams() captures configured values once during daemon startup.
 * daemonMask controls which daemon displays each parameter.
 * needFree marks values allocated by copyParams().
 */
static struct showconf_param showconfParams[] = {
    {{"LSB_CONFDIR", "LSF_CONFDIR/lsbatch"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_DEBUG", NULL}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_DEBUG_MBD", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_DEBUG_SBD", NULL}, SHOWCONF_SBD, FALSE},
    {{"LSB_JOB_CPULIMIT", NULL}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_JOB_MEMLIMIT", NULL}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_MAILPROG", "/usr/lib/sendmail"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_MAILSERVER", NULL}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_MAILSIZE_LIMIT", NULL}, SHOWCONF_SBD, FALSE},
    {{"LSB_MAILTO", "!U"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_MBD_PORT", "6881"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_MEMLIMIT_ENFORCE", "N"}, SHOWCONF_SBD, FALSE},
    {{"LSB_MIG2PEND", "0"}, SHOWCONF_MBD, FALSE},
    {{"LSB_MOD_ALL_JOBS", "N"}, SHOWCONF_MBD, FALSE},
    {{"LSB_PACK_SKIP_ERROR", "N"}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_ALIVE_TIME", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_MAX_TASK_NUM", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_PORT", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_JOB_SYNC_MODE", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_SYNC_SHM_SIZE", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_QMBD_THREAD_NUM", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_REQUEUE_TO_BOTTOM", "0"}, SHOWCONF_MBD, FALSE},
    {{"LSB_SBD_PORT", "6882"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_SET_TMPDIR", "n"}, SHOWCONF_SBD, FALSE},
    {{"LSB_SHAREDIR", "LSF_INDEP/work"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSB_SIGSTOP", NULL}, SHOWCONF_SBD, FALSE},
    {{"LSB_TIME_MBD", NULL}, SHOWCONF_MBD, FALSE},
    {{"LSB_TIME_SBD", NULL}, SHOWCONF_SBD, FALSE},
    {{"LSF_API_CONNTIMEOUT", "5"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_API_RECVTIMEOUT", "20"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_AUTH", "eauth"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_AUTH_DAEMONS", NULL}, SHOWCONF_SBD, FALSE},
    {{"LSF_BINDIR", "LSF_MACHDEP/bin"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_CONF_RETRY_INT", "30"}, SHOWCONF_LIM, FALSE},
    {{"LSF_CONF_RETRY_MAX", "0"}, SHOWCONF_LIM, FALSE},
    {{"LSF_CONFDIR", "LSF_INDEP/conf"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_DEBUG_LIM", NULL}, SHOWCONF_LIM, FALSE},
    {{"LSF_ENVDIR", "/etc"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_LIBDIR", "LSF_MACHDEP/lib"}, SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_LIM_DEBUG", NULL}, SHOWCONF_LIM, FALSE},
    {{"LSF_LIM_PORT", "6879"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_LOG_MASK", "LOG_WARNING"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_LOGDIR", "syslog"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_MASTER_LIST", NULL}, SHOWCONF_LIM, FALSE},
    {{"LSF_NON_PRIVILEGED_PORTS", "Y"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_RES_PORT", "6878"}, SHOWCONF_LIM, FALSE},
    {{"LSF_SERVERDIR", NULL}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{"LSF_UNIT_FOR_LIMITS", "MB"}, SHOWCONF_LIM | SHOWCONF_MBD | SHOWCONF_SBD, FALSE},
    {{NULL, NULL}, 0, FALSE}
};

static time_t showconfConfigTime = 0;

static struct showconf_param *findShowconfParam(const char *);
static int showconfParamIsVisible(const struct showconf_param *, int);
static int copyParams(struct config_param *);
/**
 * Find a showconf catalog entry by parameter name
 * @param[in] name: Configuration parameter name to look up
 * @return: Pointer to the matching table entry, or NULL if the name is not
 *          tracked by showconf
 */
static struct showconf_param *
findShowconfParam(const char *name)
{
    int i;

    if (name == NULL)
        return NULL;

    for (i = 0; showconfParams[i].paramInfo.paramName != NULL; i++) {
        if (strcmp(showconfParams[i].paramInfo.paramName, name) == 0)
            return &showconfParams[i];
    }

    return NULL;
}

/**
 * Check whether a defined parameter belongs to this daemon.
 * @param[in] param: Parameter entry to check.
 * @param[in] daemonMask: Daemon mask to match against.
 * @return TRUE if the param belongs to this daemon, FALSE otherwise.
 */
static int
showconfParamIsVisible(const struct showconf_param *param, int daemonMask)
{
    if (param == NULL || param->paramInfo.paramValue == NULL
        || !(param->daemonMask & daemonMask))
        return FALSE;

    return TRUE;
}

/**
 * XDR encode or decode one showconf name/value entry
 * @param[in,out] xdrs: XDR stream
 * @param[in,out] param: Parameter entry to transfer
 * @param[in] hdr: LSF message header, unused here but required by the array
 *                 element callback signature
 * @return: TRUE on success, FALSE on string transfer failure
 */
static bool_t
xdr_showConfParam(XDR *xdrs, struct config_param *param,
                  struct LSFHeader *hdr)
{
    if (!xdr_var_string(xdrs, &param->paramName)
        || !xdr_var_string(xdrs, &param->paramValue))
        return FALSE;

    return TRUE;
}

/**
 * Calculate a safe upper bound for an encoded showconf reply.
 * @param[in] reply: Reply to size; NULL returns the minimum buffer size
 * @return: Word-aligned buffer size in bytes
 */
int
xdrShowConfReplySize(const struct showConfReply *reply)
{
    int size = LSF_HEADER_LEN + 2 * NET_INTSIZE_ + NET_INTSIZE_ + 64;
    int i;

    if (reply == NULL)
        return size;

    /* Each entry is encoded as an array element plus two variable strings. */
    for (i = 0; i < reply->entryCount; i++) {
        size += NET_INTSIZE_;
        size += NET_INTSIZE_ + getXdrStrlen(reply->entries[i].paramName);
        size += NET_INTSIZE_ + getXdrStrlen(reply->entries[i].paramValue);
    }

    return ALIGNWORD_(size);
}

/**
 * XDR encode, decode, or free a showconf reply
 * @param[in,out] xdrs: XDR stream controlling ENCODE, DECODE, or FREE mode
 * @param[in,out] reply: Reply object to encode, populate, or free
 * @param[in] hdr: LSF message header passed through xdr_arrayElement()
 * @return: TRUE on success, FALSE on malformed input or allocation failure
 */
bool_t
xdr_showConfReply(XDR *xdrs, struct showConfReply *reply,
                  struct LSFHeader *hdr)
{
    int i;
    if (xdrs->x_op == XDR_FREE) {
        if (reply != NULL) {
            if (reply->entries != NULL) {
                for (i = 0; i < reply->entryCount; i++) {
                    FREEUP(reply->entries[i].paramName);
                    FREEUP(reply->entries[i].paramValue);
                }
                FREEUP(reply->entries);
            }
            reply->entryCount = 0;
        }
        return TRUE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        if (reply->entries != NULL) {
            for (i = 0; i < reply->entryCount; i++) {
                FREEUP(reply->entries[i].paramName);
                FREEUP(reply->entries[i].paramValue);
            }
            FREEUP(reply->entries);
        }
        reply->entryCount = 0;
    }

    if (!xdr_time_t(xdrs, &reply->configTime))
        return FALSE;

    if (!xdr_int(xdrs, &reply->entryCount)) {
        return FALSE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        /* Reject suspicious counts before allocating the decoded array. */
        if (reply->entryCount < 0 || reply->entryCount > 10000)
            return FALSE;

        if (reply->entryCount == 0) {
            reply->entries = NULL;
            return TRUE;
        }

        reply->entries = calloc(reply->entryCount, sizeof(struct config_param));
        if (reply->entries == NULL) {
            reply->entryCount = 0;
            return FALSE;
        }
    }

    /* xdr_showConfParam owns the per-entry name/value string transfer. */
    for (i = 0; i < reply->entryCount; i++) {
        if (!xdr_arrayElement(xdrs, (char *)&reply->entries[i], hdr,
                              xdr_showConfParam, NULL)) {
            if (xdrs->x_op == XDR_DECODE) {
                if (reply->entries != NULL) {
                    for (i = 0; i < reply->entryCount; i++) {
                        FREEUP(reply->entries[i].paramName);
                        FREEUP(reply->entries[i].paramValue);
                    }
                    FREEUP(reply->entries);
                }
                reply->entryCount = 0;
            }
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * Initialize showconf params only once.
 * @param[in] params: NULL-terminated daemon parameter array
 * @return: 0 on success, -1 on allocation failure
 */
int
initShowconfParams(struct config_param *params)
{
    static int first = TRUE;
    struct config_param envParams[2];
    char *envDir;

    if (!first){
        ls_syslog(LOG_ERR, "%s: showconf params have been initialized", __func__);
        return -1;
    }

    if (copyParams(genParams_) < 0 || copyParams(params) < 0)
        return -1;

    /* LSF_ENVDIR can come from the process environment rather than lsf.conf. */
    envDir = getenv("LSF_ENVDIR");
    if (envDir != NULL) {
        envParams[0].paramName = "LSF_ENVDIR";
        envParams[0].paramValue = envDir;
        envParams[1].paramName = NULL;
        envParams[1].paramValue = NULL;
        if (copyParams(envParams) < 0)
            return -1;
    }

    showconfConfigTime = time(NULL);
    first = FALSE;
    return 0;
}

/**
 * Release a locally built or decoded showconf reply.
 * @param[in,out] reply: Reply object to clear
 * @param[in] isDaemon: TRUE when entry strings are borrowed from the daemon's
 *                      showconf table; FALSE when XDR allocated the strings
 */
void
freeShowConfReply(struct showConfReply *reply, int isDaemon)
{
    int i;

    if (reply == NULL)
        return;

    if (reply->entries != NULL) {
        if (!isDaemon) {
            for (i = 0; i < reply->entryCount; i++) {
                FREEUP(reply->entries[i].paramName);
                FREEUP(reply->entries[i].paramValue);
            }
        }
        FREEUP(reply->entries);
    }
    reply->entryCount = 0;
}

/**
 * Build a daemon-specific showconf reply
 * The caller owns reply->entries and must release it with
 * freeShowConfReply(reply, TRUE). Entry strings borrow the showconf table.
 * @param[in] daemonMask: SHOWCONF_MBD, SHOWCONF_SBD, or SHOWCONF_LIM
 * @param[out] reply: Reply object populated with borrowed parameter names/values
 * @return: 0 on success, -1 if reply is NULL or allocation fails
 */
int
makeShowConfReply(int daemonMask, struct showConfReply *reply)
{
    int i;
    int out = 0;
    int count = 0;

    if (reply == NULL)
        return -1;

    memset(reply, 0, sizeof(*reply));
    reply->configTime = showconfConfigTime ? showconfConfigTime : time(NULL);

    /* Count first so the wire reply can use a compact contiguous array. */
    for (i = 0; showconfParams[i].paramInfo.paramName != NULL; i++) {
        if (showconfParamIsVisible(&showconfParams[i], daemonMask))
            count++;
    }

    reply->entryCount = count;
    if (reply->entryCount == 0)
        return 0;

    reply->entries = calloc(reply->entryCount, sizeof(struct config_param));
    if (reply->entries == NULL) {
        reply->entryCount = 0;
        return -1;
    }

    /* Borrow immutable daemon snapshot strings until the reply is encoded. */
    for (i = 0; showconfParams[i].paramInfo.paramName != NULL; i++) {
        if (!showconfParamIsVisible(&showconfParams[i], daemonMask))
            continue;

        reply->entries[out].paramName =
            showconfParams[i].paramInfo.paramName;
        reply->entries[out].paramValue =
            showconfParams[i].paramInfo.paramValue;
        if (reply->entries[out].paramName == NULL
            || reply->entries[out].paramValue == NULL) {
            freeShowConfReply(reply, TRUE);
            return -1;
        }
        out++;
    }

    return 0;
}

/**
 * Print a decoded showconf reply in command output format
 * @param[in] daemonName: Display name such as "MBD", "SBD", or "LIM"
 * @param[in] host: Optional host name for host-scoped daemon output
 * @param[in] reply: Decoded showconf reply to print
 */
void
printShowConfReply(const char *daemonName, const char *host,
                   const struct showConfReply *reply)
{
    char timeBuf[64];
    time_t configTime;
    int i;

    if (daemonName == NULL || reply == NULL)
        return;

    /* Be defensive for old or failed replies that carry no timestamp. */
    configTime = reply->configTime ? reply->configTime : time(NULL);
    ctime_r(&configTime, timeBuf);
    timeBuf[strcspn(timeBuf, "\n")] = '\0';

    if (host != NULL)
        fprintf(stdout, "%s configuration for host <%s> at %s\n",
                daemonName, host, timeBuf);
    else
        fprintf(stdout, "%s configuration at %s\n", daemonName, timeBuf);

    for (i = 0; i < reply->entryCount; i++) {
        fprintf(stdout, "\t%s = %s\n",
                reply->entries[i].paramName,
                reply->entries[i].paramValue ? reply->entries[i].paramValue : "");
    }
    fprintf(stdout, "\n");
}

/**
 * Deep copy tracked values and release previously owned replacements.
 * @param[in] params: Source parameter array to copy from.
 * @return: 0 on success, -1 on allocation failure.
 */
static int
copyParams(struct config_param *params)
{
    int i;
    char *copy;
    struct showconf_param *showParam;

    if (params != NULL) {
        for (i = 0; params[i].paramName != NULL; i++) {
            if (params[i].paramValue == NULL)
                continue;

            /* Only tracked parameters override the display fallback. */
            showParam = findShowconfParam(params[i].paramName);
            if (showParam == NULL)
                continue;

            copy = putstr_(params[i].paramValue);
            if (copy == NULL)
                return -1;

            if (showParam->needFree)
                FREEUP(showParam->paramInfo.paramValue);
            showParam->paramInfo.paramValue = copy;
            showParam->needFree = TRUE;
        }
    }

    return 0;
}

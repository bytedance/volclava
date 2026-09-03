/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <unistd.h>
#include <string.h>

#include "cmd.h"

#define NL_SETN 10

/* Column widths for the usage table (blimits / blimits -a) */
#define RL_NAME_LENGTH      16
#define RL_CONS_LENGTH      16
#define RL_SLOTS_LENGTH     7
#define RL_MEM_LENGTH       8
#define RL_TMP_LENGTH       8
#define RL_SWP_LENGTH       8
#define RL_JOBS_LENGTH      7
#define RL_RSRC_LENGTH      12

/* Max length for consumer value when not in -w mode */
#define RL_CONS_TRUNC_LEN   16

static char wflag = FALSE;
static char aflag = FALSE;
static char cflag = FALSE;

void
usage(char *cmd)
{
    fprintf(stderr, I18N_Usage);
    fprintf(stderr, "\n");
    fprintf(stderr, "%s [-h] [-V]\n", cmd);
    fprintf(stderr, "%s [[-a] | [-c]] [-w] [-n Limit_Name] [-P Project] "
            "[-q Queue] [-u User] [-m \"Hosts\"]\n", cmd);
    exit(1);
}

/********************************************************************************
 * getConsumerDisplay
 * Description:
 *     Get the display string for a consumer dimension. Returns "-" if the
 *     consumer is not configured, otherwise returns the formatted value.
 *     In non-wide mode, the value is truncated to RL_CONS_TRUNC_LEN.
 *
 * Input:
 *     ent [in]: the rlimitEnt
 *     type [in]: RLIMIT_CONSUMER_TYPE_*
 *
 * Return:
 *     Pointer to a static buffer containing the display string.
 ********************************************************************************/
static const char *
getConsumerDisplay(struct rlimitEnt *ent, int type)
{
    static char buf[MAXLINELEN];
    int i;

    for (i = 0; i < ent->nConsumers; i++) {
        if (ent->consumers[i].type == type) {
            if (ent->consumers[i].value == NULL
                || ent->consumers[i].value[0] == '\0') {
                return "-";
            }
            if (!wflag && strlen(ent->consumers[i].value) > RL_CONS_TRUNC_LEN) {
                /* truncate: keep the first chars and mark with trailing marker */
                snprintf(buf, sizeof(buf), "%.*s*", RL_CONS_TRUNC_LEN - 1,
                         ent->consumers[i].value);
                return buf;
            }
            return ent->consumers[i].value;
        }
    }
    return "-";
}

/********************************************************************************
 * truncIfNarrow
 * Description:
 *     In non-wide mode, truncate the string to fit the column width and mark
 *     with a trailing '*' (same convention as getConsumerDisplay). In wide
 *     mode, return the string unchanged. The returned pointer may reference
 *     a static buffer, so callers should use it before the next call.
 ********************************************************************************/
static const char *
truncIfNarrow(int colLen, const char *str)
{
    static char buf[256];

    if (wflag || str == NULL) {
        return str;
    }
    if ((int)strlen(str) <= colLen) {
        return str;
    }
    if (colLen <= 1) {
        snprintf(buf, sizeof(buf), "*");
    } else {
        snprintf(buf, sizeof(buf), "%.*s*", colLen - 1, str);
    }
    return buf;
}

/********************************************************************************
 * getResourceByName
 * Description:
 *     Find a resource entry by its name. Returns NULL if not found.
 ********************************************************************************/
static struct rlimitResourceEnt *
getResourceByName(struct rlimitEnt *ent, const char *name)
{
    int i;
    for (i = 0; i < ent->nResources; i++) {
        if (ent->resources[i].resName != NULL
            && strcasecmp(ent->resources[i].resName, name) == 0) {
            return &ent->resources[i];
        }
    }
    return NULL;
}

/********************************************************************************
 * findExternalResource
 * Description:
 *     Find an external (non-buildin) resource by name. Returns NULL if not
 *     found.
 ********************************************************************************/
static struct rlimitResourceEnt *
findExternalResource(struct rlimitEnt *ent, const char *name)
{
    int i;
    for (i = 0; i < ent->nResources; i++) {
        if (ent->resources[i].resName != NULL
            && strcmp(ent->resources[i].resName, name) == 0
            && (ent->resources[i].type == RLIMIT_TYPE_RSRC)) {
            return &ent->resources[i];
        }
    }
    return NULL;
}

/********************************************************************************
 * formatUsage
 * Description:
 *     Format a used/limit pair into a string like "1/50". If used is -1
 *     (no usage record), returns "-" for the used portion.
 *     If the limit value is 0 and percent, treat as not set.
 *
 * Input:
 *     rsrc [in]: the resource entry
 *     buf  [out]: output buffer
 *     bufLen [in]: buffer size
 ********************************************************************************/
static void
formatUsage(struct rlimitResourceEnt *rsrc, char *buf, int bufLen)
{
    if (rsrc == NULL) {
        strcpy(buf, "-");
        return;
    }

    /* For integer resources (JOBS/SLOTS), show integer */
    if (rsrc->resName != NULL
        && (strcmp(rsrc->resName, "JOBS") == 0
            || strcmp(rsrc->resName, "SLOTS") == 0)) {
        if (rsrc->used < 0) {
            snprintf(buf, bufLen, "-/%d", (int)rsrc->value);
        } else {
            snprintf(buf, bufLen, "%d/%d", (int)rsrc->used, (int)rsrc->value);
        }
        return;
    }

    /* SLOTS_PER_PROCESSOR may be fractional (e.g. 0.5); show integer when
     * no decimal part, otherwise one decimal place. */
    if (rsrc->resName != NULL
        && strcmp(rsrc->resName, "SLOTS_PER_PROCESSOR") == 0) {
        if (rsrc->value == (float)(int)rsrc->value) {
            if (rsrc->used < 0) {
                snprintf(buf, bufLen, "-/%d", (int)rsrc->value);
            } else {
                snprintf(buf, bufLen, "%d/%d",
                         (int)rsrc->used, (int)rsrc->value);
            }
        } else {
            if (rsrc->used < 0) {
                snprintf(buf, bufLen, "-/%.1f", rsrc->value);
            } else {
                snprintf(buf, bufLen, "%.1f/%.1f",
                         rsrc->used, rsrc->value);
            }
        }
        return;
    }

    /* For MEM/SWP/TMP/external resources, show float with appropriate precision.
     * When used < 0 (no usage record) and the resource is percent-defined,
     * the value field holds the raw configured percentage; show it with a %
     * suffix. When used >= 0, value is the absolute limit (MB) regardless of
     * isPercent, so no % suffix. */
    if (rsrc->used < 0) {
        if (rsrc->isPercent) {
            snprintf(buf, bufLen, "-/%.0f%%", rsrc->value);
        } else {
            snprintf(buf, bufLen, "-/%.0f", rsrc->value);
        }
    } else {
        snprintf(buf, bufLen, "%.0f/%.0f", rsrc->used, rsrc->value);
    }
}

/********************************************************************************
 * printConfig
 * Description:
 *     Print resource limit configurations in the long "Begin Limit ... End limit"
 *     format used by blimits -c.
 *
 * Input:
 *     limits [in]: array of rlimitEnt
 *     numLimits [in]: count
 ********************************************************************************/
static void
printConfig(struct rlimitEnt *limits, int numLimits)
{
    int i, j, k;

    for (i = 0; i < numLimits; i++) {
        struct rlimitEnt *ent = &limits[i];
        char resBuf[MAXLINELEN];

        printf(" Begin Limit \n");
        printf("   %-13s = %s \n", "NAME", ent->name ? ent->name : "");

        /* consumers: print in order HOSTS, QUEUES, USERS, PROJECTS.
         * value is pre-formatted server-side as SHARED(...)/PER(...). */
        for (k = 0; k < RLIMIT_CONSUMER_POSITION_MAX; k++) {
            const char *label = NULL;

            switch (k) {
            case RLIMIT_CONSUMER_TYPE_HOST:
                label = "HOSTS";
                break;
            case RLIMIT_CONSUMER_TYPE_QUEUE:
                label = "QUEUES";
                break;
            case RLIMIT_CONSUMER_TYPE_USER:
                label = "USERS";
                break;
            case RLIMIT_CONSUMER_TYPE_PROJECT:
                label = "PROJECTS";
                break;
            }

            for (j = 0; j < ent->nConsumers; j++) {
                if (ent->consumers[j].type != k) {
                    continue;
                }
                if (ent->consumers[j].value == NULL
                    || ent->consumers[j].value[0] == '\0') {
                    continue;
                }
                printf("   %-13s = %s \n", label, ent->consumers[j].value);
            }
        }

        /* all resources (built-in and custom) in a single RESOURCES= field,
         * formatted as [name,value] [name,value] ... with a space between
         * brackets and no space after the comma inside. For percentage
         * values, % follows the number. */
        resBuf[0] = '\0';
        for (j = 0; j < ent->nResources; j++) {
            struct rlimitResourceEnt *r = &ent->resources[j];
            char valBuf[64];
            const char *resName = NULL;
            char rbuf[128];

            switch (r->type) {
            case RLIMIT_TYPE_JOBS:
                resName = "JOBS";
                break;
            case RLIMIT_TYPE_SLOTS:
                resName = "SLOTS";
                break;
            case RLIMIT_TYPE_MEM:
                resName = "MEM";
                break;
            case RLIMIT_TYPE_SWP:
                resName = "SWP";
                break;
            case RLIMIT_TYPE_TMP:
                resName = "TMP";
                break;
            case RLIMIT_TYPE_SLOTS_PER_PROCESSOR:
                resName = "SLOTS_PER_PROCESSOR";
                break;
            default:
                resName = r->resName ? r->resName : "";
                break;
            }

            if (r->value == (float)(int)r->value) {
                snprintf(valBuf, sizeof(valBuf), "%.0f%s",
                         r->value, r->isPercent ? "%" : "");
            } else {
                snprintf(valBuf, sizeof(valBuf), "%.1f%s",
                         r->value, r->isPercent ? "%" : "");
            }

            snprintf(rbuf, sizeof(rbuf), "[%s,%s]", resName, valBuf);
            if (resBuf[0] != '\0') {
                strcat(resBuf, " ");
            }
            strcat(resBuf, rbuf);
        }
        if (resBuf[0] != '\0') {
            printf("   %-13s = %s \n", "RESOURCES", resBuf);
        }

        if (ent->desc && ent->desc[0] != '\0') {
            printf("   %-13s = %s \n", "DESCRIPTION", ent->desc);
        }
        printf(" End Limit \n");
        if (i < numLimits - 1) {
            printf("\n");
        }
    }
}

/********************************************************************************
 * collectExternalResourceNames
 * Description:
 *     Walk all limits and collect the unique names of external (non-buildin)
 *     resources. Returns the count and fills the names array (caller must free).
 *
 * Input:
 *     limits [in]: array of rlimitEnt
 *     numLimits [in]: count
 *     names [out]: pointer to dynamically-allocated array of name strings
 *
 * Return:
 *     Number of unique external resource names, 0 if none.
 ********************************************************************************/
static int
collectExternalResourceNames(struct rlimitEnt *limits, int numLimits,
                             char ***namesOut)
{
    int i, j, k;
    int count = 0;
    int cap = 16;
    char **names;

    *namesOut = NULL;
    if (numLimits <= 0) {
        return 0;
    }

    names = (char **)calloc(cap, sizeof(char *));
    if (!names) {
        return 0;
    }

    for (i = 0; i < numLimits; i++) {
        struct rlimitEnt *ent = &limits[i];
        for (j = 0; j < ent->nResources; j++) {
            if (ent->resources[j].type != RLIMIT_TYPE_RSRC) {
                continue;
            }
            if (ent->resources[j].resName == NULL) {
                continue;
            }
            /* skip if already collected */
            for (k = 0; k < count; k++) {
                if (strcmp(names[k], ent->resources[j].resName) == 0) {
                    break;
                }
            }
            if (k < count) {
                continue;
            }
            /* add new name */
            if (count >= cap) {
                char **tmp;
                cap *= 2;
                tmp = (char **)realloc(names, cap * sizeof(char *));
                if (!tmp) {
                    *namesOut = names;
                    return count;
                }
                names = tmp;
            }
            names[count] = strdup(ent->resources[j].resName);
            if (names[count]) {
                count++;
            }
        }
    }

    *namesOut = names;
    return count;
}

/********************************************************************************
 * printUsageTable
 * Description:
 *     Print the usage table (blimits / blimits -a). Output is split into two
 *     regions: INTERNAL RESOURCE LIMITS (buildin resources SLOTS/MEM/TMP/SWP/
 *     JOBS) and EXTERNAL RESOURCE LIMITS (custom resources).
 *
 *     For SLOTS column: use SLOTS resource if present, else fall back to
 *     SLOTS_PER_PROCESSOR, else "-".
 *
 * Input:
 *     limits [in]: array of rlimitEnt
 *     numLimits [in]: count
 ********************************************************************************/
static void
printUsageTable(struct rlimitEnt *limits, int numLimits)
{
    int i, j, k;
    char **extNames = NULL;
    int numExt = 0;
    int haveInternal = FALSE;
    int haveExternal = FALSE;

    numExt = collectExternalResourceNames(limits, numLimits, &extNames);

    /* determine if there are any internal/external resources to print */
    for (i = 0; i < numLimits; i++) {
        struct rlimitEnt *ent = &limits[i];
        for (j = 0; j < ent->nResources; j++) {
            if (ent->resources[j].type != RLIMIT_TYPE_RSRC) {
                haveInternal = TRUE;
            } else {
                haveExternal = TRUE;
            }
        }
    }

    /* INTERNAL RESOURCE LIMITS section */
    if (haveInternal) {
        printf("INTERNAL RESOURCE LIMITS:\n\n");

        /* header */
        prtWord2(RL_NAME_LENGTH, "NAME", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "USERS", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "QUEUES", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "HOSTS", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "PROJECTS", 1); putchar(' ');
        prtWord2(RL_SLOTS_LENGTH, "SLOTS", 1); putchar(' ');
        prtWord2(RL_MEM_LENGTH, "MEM", 1); putchar(' ');
        prtWord2(RL_TMP_LENGTH, "TMP", 1); putchar(' ');
        prtWord2(RL_SWP_LENGTH, "SWP", 1); putchar(' ');
        prtWord2(RL_JOBS_LENGTH, "JOBS", 1); putchar(' ');
        printf("\n");

        for (i = 0; i < numLimits; i++) {
            struct rlimitEnt *ent = &limits[i];
            struct rlimitResourceEnt *slotsR, *memR, *tmpR, *swpR, *jobsR;
            char slotsBuf[32], memBuf[32], tmpBuf[32], swpBuf[32], jobsBuf[32];

            slotsR = getResourceByName(ent, "SLOTS");
            if (slotsR == NULL) {
                slotsR = getResourceByName(ent, "SLOTS_PER_PROCESSOR");
            }
            memR = getResourceByName(ent, "MEM");
            tmpR = getResourceByName(ent, "TMP");
            swpR = getResourceByName(ent, "SWP");
            jobsR = getResourceByName(ent, "JOBS");

            formatUsage(slotsR, slotsBuf, sizeof(slotsBuf));
            formatUsage(memR, memBuf, sizeof(memBuf));
            formatUsage(tmpR, tmpBuf, sizeof(tmpBuf));
            formatUsage(swpR, swpBuf, sizeof(swpBuf));
            formatUsage(jobsR, jobsBuf, sizeof(jobsBuf));

            prtWord2(RL_NAME_LENGTH, ent->name ? ent->name : "", 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_USER), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_QUEUE), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_HOST), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_PROJECT), 1); putchar(' ');
            prtWord2(RL_SLOTS_LENGTH, truncIfNarrow(RL_SLOTS_LENGTH, slotsBuf), 1); putchar(' ');
            prtWord2(RL_MEM_LENGTH, truncIfNarrow(RL_MEM_LENGTH, memBuf), 1); putchar(' ');
            prtWord2(RL_TMP_LENGTH, truncIfNarrow(RL_TMP_LENGTH, tmpBuf), 1); putchar(' ');
            prtWord2(RL_SWP_LENGTH, truncIfNarrow(RL_SWP_LENGTH, swpBuf), 1); putchar(' ');
            prtWord2(RL_JOBS_LENGTH, truncIfNarrow(RL_JOBS_LENGTH, jobsBuf), 1); putchar(' ');
            printf("\n");
        }
    }

    /* EXTERNAL RESOURCE LIMITS section */
    if (haveExternal && numExt > 0) {
        if (haveInternal) {
            printf("\n");
        }
        printf("EXTERNAL RESOURCE LIMITS:\n\n");

        /* header */
        prtWord2(RL_NAME_LENGTH, "NAME", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "USERS", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "QUEUES", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "HOSTS", 1); putchar(' ');
        prtWord2(RL_CONS_LENGTH, "PROJECTS", 1); putchar(' ');
        for (k = 0; k < numExt; k++) {
            prtWord2(RL_RSRC_LENGTH, extNames[k], 1); putchar(' ');
        }
        printf("\n");

        for (i = 0; i < numLimits; i++) {
            struct rlimitEnt *ent = &limits[i];

            prtWord2(RL_NAME_LENGTH, ent->name ? ent->name : "", 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_USER), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_QUEUE), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_HOST), 1); putchar(' ');
            prtWord2(RL_CONS_LENGTH, getConsumerDisplay(ent, RLIMIT_CONSUMER_TYPE_PROJECT), 1); putchar(' ');

            for (k = 0; k < numExt; k++) {
                struct rlimitResourceEnt *r;
                char rbuf[32];
                r = findExternalResource(ent, extNames[k]);
                formatUsage(r, rbuf, sizeof(rbuf));
                prtWord2(RL_RSRC_LENGTH, truncIfNarrow(RL_RSRC_LENGTH, rbuf), 1); putchar(' ');
            }
            printf("\n");
        }
    }

    /* free external names */
    if (extNames) {
        for (k = 0; k < numExt; k++) {
            free(extNames[k]);
        }
        free(extNames);
    }
}

/********************************************************************************
 * sortLimitsByName
 * Description:
 *     Sort the limits array by name using shell sort (stable enough for this
 *     purpose). Mirrors the sort_users pattern in busers.c.
 ********************************************************************************/
static void
sortLimitsByName(struct rlimitEnt *limits, int numLimits)
{
    int i, j, k;
    struct rlimitEnt tmp;

    for (k = numLimits / 2; k > 0; k /= 2) {
        for (i = k; i < numLimits; i++) {
            for (j = i - k; j >= 0; j -= k) {
                if (limits[j].name && limits[j+k].name
                    && strcmp(limits[j].name, limits[j+k].name) < 0) {
                    break;
                }
                tmp = limits[j];
                limits[j] = limits[j+k];
                limits[j+k] = tmp;
            }
        }
    }
}

int
main(int argc, char **argv)
{
    int cc;
    int i;
    int numLimits = 0;
    struct rlimitEnt *limits = NULL;
    int options = 0;
    char **names = NULL;
    int numNames = 0;
    char *queue = NULL;
    char *user = NULL;
    char *project = NULL;
    char *hosts = NULL;
    char *optarg_m = NULL;
    char *optarg_n = NULL;
    char *optarg_P = NULL;
    char *optarg_q = NULL;
    char *optarg_u = NULL;

    _i18n_init(I18N_CAT_MIN);

    if (lsb_init(argv[0]) < 0) {
        lsb_perror("lsb_init");
        _i18n_end(ls_catd);
        exit(1);
    }

    /* pre-scan for -h and -V like bhosts does */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            _i18n_end(ls_catd);
            exit(0);
        } else if (strcmp(argv[i], "-V") == 0) {
            fputs(_LS_VERSION_, stdout);
            _i18n_end(ls_catd);
            exit(0);
        }
    }

    while ((cc = getopt(argc, argv, "acwn:P:q:u:m:")) != EOF) {
        switch (cc) {
        case 'a':
            aflag = TRUE;
            break;
        case 'c':
            cflag = TRUE;
            break;
        case 'w':
            wflag = TRUE;
            break;
        case 'n':
            optarg_n = optarg;
            break;
        case 'P':
            optarg_P = optarg;
            break;
        case 'q':
            optarg_q = optarg;
            break;
        case 'u':
            optarg_u = optarg;
            break;
        case 'm':
            optarg_m = optarg;
            break;
        default:
            usage(argv[0]);
        }
    }

    /* -a and -c cannot be used together */
    if (aflag && cflag) {
        fprintf(stderr, "%s: -a and -c cannot be used together\n", argv[0]);
        usage(argv[0]);
    }

    /* build the request filters */
    if (optarg_n) {
        /* split by whitespace into names array.
         * NOTE: getNextWord_() returns a pointer into a STATIC buffer that
         * is overwritten on each call, so we must strdup() each word. */
        char buf[MAXLINELEN];
        char *sp, *word;
        int cap = 8;
        snprintf(buf, sizeof(buf), "%s", optarg_n);
        sp = buf;
        names = (char **)calloc(cap, sizeof(char *));
        if (!names) {
            lsberrno = LSBE_NO_MEM;
            lsb_perror("calloc");
            exit(1);
        }
        while ((word = getNextWord_(&sp)) != NULL) {
            if (numNames >= cap) {
                cap *= 2;
                names = (char **)realloc(names, cap * sizeof(char *));
                if (!names) {
                    lsberrno = LSBE_NO_MEM;
                    lsb_perror("realloc");
                    exit(1);
                }
            }
            names[numNames] = strdup(word);
            if (names[numNames] == NULL) {
                lsberrno = LSBE_NO_MEM;
                lsb_perror("strdup");
                exit(1);
            }
            numNames++;
        }
    }
    queue = optarg_q;
    user = optarg_u;
    project = optarg_P;
    hosts = optarg_m;

    /* determine request options */
    if (cflag) {
        options = RLIMIT_OPT_CONFIG_ONLY;
    } else if (aflag) {
        options = RLIMIT_OPT_ALL;
    } else {
        options = 0;  /* default: only limits with active usage */
    }

    limits = lsb_rsrclimitinfo(options, numNames, names,
                              queue, user, project, hosts, &numLimits);
    if (limits == NULL) {
        if (lsberrno == LSBE_NO_ERROR) {
            if (cflag || aflag) {
                printf("No resource configuration found.\n");
            } else {
                printf("No resource usage found.\n");
            }
            _i18n_end(ls_catd);
            exit(0);
        }
        lsb_perror(NULL);
        if (names) {
            for (i = 0; i < numNames; i++) {
                FREEUP(names[i]);
            }
            free(names);
        }
        _i18n_end(ls_catd);
        exit(1);
    }

    if (numLimits > 1) {
        sortLimitsByName(limits, numLimits);
    }

    if (numLimits == 0) {
        if (cflag || aflag) {
            printf("No resource configuration found.\n");
        } else {
            printf("No resource usage found.\n");
        }
    } else if (cflag) {
        printConfig(limits, numLimits);
    } else {
        printUsageTable(limits, numLimits);
    }

    if (names) {
        for (i = 0; i < numNames; i++) {
            FREEUP(names[i]);
        }
        free(names);
    }
    _i18n_end(ls_catd);
    exit(0);
}

/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#ifndef OUTPUT_FIELDS_H
#define OUTPUT_FIELDS_H
#include "fmtout.h"

/* Canonical names, aliases, rendering metadata, and wire dependencies share
 * one registry. Wire bits are stable within the typed output protocol and use
 * a 64-bit mask (1ULL << N) to selectively project fields across daemon RPCs.
 */

static const struct fmt_field_def output_job_fields[] = {
    {"JOBID", "ID", "JOBID", 7, 0, NULL, 0},
    {"JOB_IDX", "JOBINDEX", "JOB_IDX", 8, 0, "JOBINDEX", 0},
    {"USER", NULL, "USER", 7, 0, NULL, (1ULL << 0)},
    {"STAT", NULL, "STAT", 5, 0, NULL, (1ULL << 1) | (1ULL << 15)},
    {"QUEUE", NULL, "QUEUE", 10, 0, NULL, (1ULL << 2)},
    {"FROM_HOST", NULL, "FROM_HOST", 11, 0, NULL, (1ULL << 3)},
    {"EXEC_HOST", NULL, "EXEC_HOST", 11, 0, NULL, (1ULL << 4) | (1ULL << 1)},
    {"JOB_NAME", "NAME", "JOB_NAME", 10, FMT_FIELD_KEEP_TAIL, NULL, (1ULL << 5)},
    {"SUBMIT_TIME", NULL, "SUBMIT_TIME", 15, 0, NULL, (1ULL << 6)},
    {"PROJ_NAME", NULL, "PROJ_NAME", 11, 0, NULL, (1ULL << 7)},
    {"CPU_USED", NULL, "CPU_USED", 10, 0, NULL, (1ULL << 8)},
    {"MEM", NULL, "MEM", 15, 0, NULL, (1ULL << 9)},
    {"SWAP", NULL, "SWAP", 15, 0, NULL, (1ULL << 10)},
    {"MAX_MEM", NULL, "MAX_MEM", 15, 0, NULL, (1ULL << 16)},
    {"AVG_MEM", NULL, "AVG_MEM", 15, 0, NULL, (1ULL << 17)},
    {"PIDS", NULL, "PIDS", 20, 0, NULL, (1ULL << 11)},
    {"START_TIME", NULL, "START_TIME", 15, 0, NULL, (1ULL << 12)},
    {"FINISH_TIME", NULL, "FINISH_TIME", 16, 0, NULL, (1ULL << 13)},
    {"EXIT_CODE", NULL, "EXIT_CODE", 10, 0, NULL, (1ULL << 14) | (1ULL << 1)}
};

static const struct fmt_field_def output_host_fields[] = {
    {"HOST_NAME", "HNAME", "HOST_NAME", 20, 0, NULL, (1ULL << 0)},
    {"STATUS", "STAT", "STATUS", 15, 0, NULL, (1ULL << 1)},
    {"JL_U", "JLU", "JL/U", 8, 0, NULL, (1ULL << 2)},
    {"MAX", NULL, "MAX", 8, 0, NULL, (1ULL << 3)},
    {"NJOBS", NULL, "NJOBS", 8, 0, NULL, (1ULL << 4)},
    {"RUN", NULL, "RUN", 8, 0, NULL, (1ULL << 5)},
    {"SSUSP", NULL, "SSUSP", 8, 0, NULL, (1ULL << 6)},
    {"USUSP", NULL, "USUSP", 8, 0, NULL, (1ULL << 7)},
    {"RSV", NULL, "RSV", 8, 0, NULL, (1ULL << 8)},
    {"DISPATCH_WINDOW", "DISPWIN", "DISPATCH_WINDOW", 50, 0, NULL, (1ULL << 9)},
    {"AVAILABLE_MEM", NULL, "AVAILABLE_MEM", 15, 0, NULL, (1ULL << 10)},
    {"RESERVED_MEM", NULL, "RESERVED_MEM", 15, 0, NULL, (1ULL << 11)},
    {"TOTAL_MEM", NULL, "TOTAL_MEM", 15, 0, NULL, (1ULL << 12)}
};

static const struct fmt_field_def output_queue_fields[] = {
    {"QUEUE_NAME", "QNAME", "QUEUE_NAME", 15, 0, NULL, (1ULL << 0)},
    {"DESCRIPTION", "DESC", "DESCRIPTION", 50, 0, NULL, (1ULL << 1)},
    {"PRIORITY", "PRIO", "PRIORITY", 10, 0, NULL, (1ULL << 2)},
    {"STATUS", "STAT", "STATUS", 12, 0, NULL, (1ULL << 3)},
    {"MAX", NULL, "MAX", 10, 0, NULL, (1ULL << 4)},
    {"JL_U", "JLU", "JL/U", 10, 0, NULL, (1ULL << 5)},
    {"JL_P", "JLP", "JL/P", 10, 0, NULL, (1ULL << 6)},
    {"JL_H", "JLH", "JL/H", 10, 0, NULL, (1ULL << 7)},
    {"NJOBS", NULL, "NJOBS", 10, 0, NULL, (1ULL << 8)},
    {"PEND", NULL, "PEND", 10, 0, NULL, (1ULL << 9)},
    {"RUN", NULL, "RUN", 10, 0, NULL, (1ULL << 10)},
    {"SUSP", NULL, "SUSP", 10, 0, NULL, (1ULL << 11)},
    {"RSV", NULL, "RSV", 10, 0, NULL, (1ULL << 12)},
    {"USUSP", NULL, "USUSP", 10, 0, NULL, (1ULL << 13)},
    {"SSUSP", NULL, "SSUSP", 10, 0, NULL, (1ULL << 14)},
    {"NICE", NULL, "NICE", 6, 0, NULL, (1ULL << 15)},
    {"HOSTS", NULL, "HOSTS", 50, 0, NULL, (1ULL << 16)},
    {"RES_REQ", NULL, "RES_REQ", 20, 0, NULL, (1ULL << 17)},
    {"MAX_CORELIMIT", "CORELIMIT", "MAX_CORELIMIT", 8, 0, NULL, (1ULL << 18)},
    {"MAX_CPULIMIT", "CPULIMIT", "MAX_CPULIMIT", 30, 0, NULL, (1ULL << 19)},
    {"DEFAULT_CPULIMIT", "DEF_CPULIMIT", "DEFAULT_CPULIMIT", 30, 0, NULL, (1ULL << 20)},
    {"MAX_DATALIMIT", "DATALIMIT", "MAX_DATALIMIT", 8, 0, NULL, (1ULL << 21)},
    {"DEFAULT_DATALIMIT", "DEF_DATALIMIT", "DEFAULT_DATALIMIT", 8, 0, NULL, (1ULL << 22)},
    {"MAX_FILELIMIT", "FILELIMIT", "MAX_FILELIMIT", 8, 0, NULL, (1ULL << 23)},
    {"MAX_MEMLIMIT", "MEMLIMIT", "MAX_MEMLIMIT", 8, 0, NULL, (1ULL << 24)},
    {"DEFAULT_MEMLIMIT", "DEF_MEMLIMIT", "DEFAULT_MEMLIMIT", 8, 0, NULL, (1ULL << 25)},
    {"MAX_PROCESSLIMIT", "PROCESSLIMIT", "MAX_PROCESSLIMIT", 8, 0, NULL, (1ULL << 26)},
    {"DEFAULT_PROCESSLIMIT", "DEF_PROCESSLIMIT", "DEFAULT_PROCESSLIMIT", 8, 0, NULL, (1ULL << 27)},
    {"MAX_STACKLIMIT", "STACKLIMIT", "MAX_STACKLIMIT", 8, 0, NULL, (1ULL << 28)},
    {"MAX_SWAPLIMIT", "SWAPLIMIT", "MAX_SWAPLIMIT", 8, 0, NULL, (1ULL << 29)},
    {"MAX_TASKLIMIT", "TASKLIMIT", "MAX_TASKLIMIT", 6, 0, NULL, (1ULL << 30)},
    {"MIN_TASKLIMIT", NULL, "MIN_TASKLIMIT", 6, 0, NULL, (1ULL << 31)},
    {"DEFAULT_TASKLIMIT", "DEF_TASKLIMIT", "DEFAULT_TASKLIMIT", 6, 0, NULL, (1ULL << 32)}
};

static const struct fmt_field_def output_lim_fields[] = {
    {"HOST_NAME", "HNAME", "HOST_NAME", 20, 0, NULL, (1ULL << 0)},
    {"TYPE", NULL, "type", 10, 0, NULL, (1ULL << 1)},
    {"MODEL", NULL, "model", 10, 0, NULL, (1ULL << 2)},
    {"CPUF", NULL, "cpuf", 10, 0, NULL, (1ULL << 3)},
    {"NCPUS", NULL, "ncpus", 8, 0, NULL, (1ULL << 4)},
    {"MAXMEM", NULL, "maxmem", 10, 0, NULL, (1ULL << 5)},
    {"MAXSWP", NULL, "maxswp", 10, 0, NULL, (1ULL << 6)},
    {"SERVER", NULL, "server", 10, 0, NULL, (1ULL << 7)},
    {"RESOURCES", "RES", "RESOURCES", 20, 0, NULL, (1ULL << 8)},
    {"MAXTMP", NULL, "maxtmp", 10, 0, NULL, (1ULL << 9)},
    {"NPROCS", NULL, "nprocs", 8, 0, NULL, (1ULL << 10)},
    {"RUN_WINDOWS", "RUNWIN", "RUN_WINDOWS", 20, 0, NULL, (1ULL << 11)}
};

#define OUTPUT_FIELD_COUNT(table) ((int)(sizeof(table) / sizeof((table)[0])))
#define OUTPUT_MASK(fields, group) \
    fmt_fields_mask(fields, output_##group##_fields, OUTPUT_FIELD_COUNT(output_##group##_fields))

#define OUTPUT_HOST_MEMBERS(X) \
    X(((1ULL << 0)), string, host) \
    X(((1ULL << 1) | (1ULL << 10) | (1ULL << 11) | (1ULL << 12)), int, hStatus) \
    X(((1ULL << 2)), int, userJobLimit) \
    X(((1ULL << 3)), int, maxJobs) \
    X(((1ULL << 4)), int, numJobs) \
    X(((1ULL << 5)), int, numRUN) \
    X(((1ULL << 6)), int, numSSUSP) \
    X(((1ULL << 7)), int, numUSUSP) \
    X(((1ULL << 8)), int, numRESERVE) \
    X(((1ULL << 9)), string, windows)
#define OUTPUT_QUEUE_MEMBERS(X) \
    X(((1ULL << 0)), string, queue) \
    X(((1ULL << 1)), string, description) \
    X(((1ULL << 2)), int, priority) \
    X(((1ULL << 3)), int, qStatus) \
    X(((1ULL << 4)), int, maxJobs) \
    X(((1ULL << 5)), int, userJobLimit) \
    X(((1ULL << 6)), float, procJobLimit) \
    X(((1ULL << 7)), int, hostJobLimit) \
    X(((1ULL << 8)), int, numJobs) \
    X(((1ULL << 9)), int, numPEND) \
    X(((1ULL << 10)), int, numRUN) \
    X(((1ULL << 11) | (1ULL << 14)), int, numSSUSP) \
    X(((1ULL << 11) | (1ULL << 13)), int, numUSUSP) \
    X(((1ULL << 12)), int, numRESERVE) \
    X(((1ULL << 15)), short, nice) \
    X(((1ULL << 16)), string, hostList) \
    X(((1ULL << 17)), string, resReq) \
    X(((1ULL << 18)), int, rLimits[LSF_RLIMIT_CORE]) \
    X(((1ULL << 19)), int, rLimits[LSF_RLIMIT_CPU]) \
    X(((1ULL << 20)), int, defLimits[LSF_RLIMIT_CPU]) \
    X(((1ULL << 21)), int, rLimits[LSF_RLIMIT_DATA]) \
    X(((1ULL << 22)), int, defLimits[LSF_RLIMIT_DATA]) \
    X(((1ULL << 23)), int, rLimits[LSF_RLIMIT_FSIZE]) \
    X(((1ULL << 24)), int, rLimits[LSF_RLIMIT_RSS]) \
    X(((1ULL << 25)), int, defLimits[LSF_RLIMIT_RSS]) \
    X(((1ULL << 26)), int, rLimits[LSF_RLIMIT_PROCESS]) \
    X(((1ULL << 27)), int, defLimits[LSF_RLIMIT_PROCESS]) \
    X(((1ULL << 28)), int, rLimits[LSF_RLIMIT_STACK]) \
    X(((1ULL << 29)), int, rLimits[LSF_RLIMIT_SWAP]) \
    X(((1ULL << 30)), int, procLimit) \
    X(((1ULL << 31)), int, minProcLimit) \
    X(((1ULL << 32)), int, defProcLimit)
#endif

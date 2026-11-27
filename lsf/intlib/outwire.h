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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#ifndef OUTWIRE_H
#define OUTWIRE_H

#include <stdint.h>

/*
 * Pure wire protocol bit definitions for custom output RPCs.
 * Independent of formatter and cJSON to ensure protocol layering cleanliness.
 */

/* Job output fields wire bits */
#define JOB_OUTPUT_USER        (UINT64_C(1) << 0)
#define JOB_OUTPUT_STAT        (UINT64_C(1) << 1)
#define JOB_OUTPUT_QUEUE       (UINT64_C(1) << 2)
#define JOB_OUTPUT_FROM_HOST   (UINT64_C(1) << 3)
#define JOB_OUTPUT_EXEC_HOST   (UINT64_C(1) << 4)
#define JOB_OUTPUT_JOB_NAME    (UINT64_C(1) << 5)
#define JOB_OUTPUT_SUBMIT_TIME (UINT64_C(1) << 6)
#define JOB_OUTPUT_PROJ_NAME   (UINT64_C(1) << 7)
#define JOB_OUTPUT_CPU_USED    (UINT64_C(1) << 8)
#define JOB_OUTPUT_MEM         (UINT64_C(1) << 9)
#define JOB_OUTPUT_SWAP        (UINT64_C(1) << 10)
#define JOB_OUTPUT_PIDS        (UINT64_C(1) << 11)
#define JOB_OUTPUT_START_TIME  (UINT64_C(1) << 12)
#define JOB_OUTPUT_FINISH_TIME (UINT64_C(1) << 13)
#define JOB_OUTPUT_EXIT_CODE   (UINT64_C(1) << 14)
#define JOB_OUTPUT_REASONS     (UINT64_C(1) << 15)
#define JOB_OUTPUT_MAX_MEM     (UINT64_C(1) << 16)
#define JOB_OUTPUT_AVG_MEM     (UINT64_C(1) << 17)
#define JOB_OUTPUT_HOST_SPEC   (UINT64_C(1) << 18)
#define JOB_OUTPUT_VALID_MASK \
    (JOB_OUTPUT_USER | JOB_OUTPUT_STAT | \
     JOB_OUTPUT_QUEUE | JOB_OUTPUT_FROM_HOST | \
     JOB_OUTPUT_EXEC_HOST | JOB_OUTPUT_JOB_NAME | \
     JOB_OUTPUT_SUBMIT_TIME | JOB_OUTPUT_PROJ_NAME | \
     JOB_OUTPUT_CPU_USED | JOB_OUTPUT_MEM | \
     JOB_OUTPUT_SWAP | JOB_OUTPUT_PIDS | \
     JOB_OUTPUT_START_TIME | JOB_OUTPUT_FINISH_TIME | \
     JOB_OUTPUT_EXIT_CODE | JOB_OUTPUT_REASONS | \
     JOB_OUTPUT_MAX_MEM | JOB_OUTPUT_AVG_MEM | \
     JOB_OUTPUT_HOST_SPEC)

/* Host output fields wire bits */
#define HOST_OUTPUT_HOST_NAME       (UINT64_C(1) << 0)
#define HOST_OUTPUT_STATUS          (UINT64_C(1) << 1)
#define HOST_OUTPUT_JL_U            (UINT64_C(1) << 2)
#define HOST_OUTPUT_MAX             (UINT64_C(1) << 3)
#define HOST_OUTPUT_NJOBS           (UINT64_C(1) << 4)
#define HOST_OUTPUT_RUN             (UINT64_C(1) << 5)
#define HOST_OUTPUT_SSUSP           (UINT64_C(1) << 6)
#define HOST_OUTPUT_USUSP           (UINT64_C(1) << 7)
#define HOST_OUTPUT_RSV             (UINT64_C(1) << 8)
#define HOST_OUTPUT_DISPATCH_WINDOW (UINT64_C(1) << 9)
#define HOST_OUTPUT_AVAILABLE_MEM   (UINT64_C(1) << 10)
#define HOST_OUTPUT_RESERVED_MEM    (UINT64_C(1) << 11)
#define HOST_OUTPUT_TOTAL_MEM       (UINT64_C(1) << 12)

/* Queue output fields wire bits */
#define QUEUE_OUTPUT_QUEUE_NAME           (UINT64_C(1) << 0)
#define QUEUE_OUTPUT_DESCRIPTION          (UINT64_C(1) << 1)
#define QUEUE_OUTPUT_PRIORITY             (UINT64_C(1) << 2)
#define QUEUE_OUTPUT_STATUS               (UINT64_C(1) << 3)
#define QUEUE_OUTPUT_MAX                  (UINT64_C(1) << 4)
#define QUEUE_OUTPUT_JL_U                 (UINT64_C(1) << 5)
#define QUEUE_OUTPUT_JL_P                 (UINT64_C(1) << 6)
#define QUEUE_OUTPUT_JL_H                 (UINT64_C(1) << 7)
#define QUEUE_OUTPUT_NJOBS                (UINT64_C(1) << 8)
#define QUEUE_OUTPUT_PEND                 (UINT64_C(1) << 9)
#define QUEUE_OUTPUT_RUN                  (UINT64_C(1) << 10)
#define QUEUE_OUTPUT_SUSP                 (UINT64_C(1) << 11)
#define QUEUE_OUTPUT_RSV                  (UINT64_C(1) << 12)
#define QUEUE_OUTPUT_USUSP                (UINT64_C(1) << 13)
#define QUEUE_OUTPUT_SSUSP                (UINT64_C(1) << 14)
#define QUEUE_OUTPUT_NICE                 (UINT64_C(1) << 15)
#define QUEUE_OUTPUT_HOSTS                (UINT64_C(1) << 16)
#define QUEUE_OUTPUT_RES_REQ              (UINT64_C(1) << 17)
#define QUEUE_OUTPUT_MAX_CORELIMIT        (UINT64_C(1) << 18)
#define QUEUE_OUTPUT_MAX_CPULIMIT         (UINT64_C(1) << 19)
#define QUEUE_OUTPUT_DEFAULT_CPULIMIT     (UINT64_C(1) << 20)
#define QUEUE_OUTPUT_MAX_DATALIMIT        (UINT64_C(1) << 21)
#define QUEUE_OUTPUT_DEFAULT_DATALIMIT    (UINT64_C(1) << 22)
#define QUEUE_OUTPUT_MAX_FILELIMIT        (UINT64_C(1) << 23)
#define QUEUE_OUTPUT_MAX_MEMLIMIT         (UINT64_C(1) << 24)
#define QUEUE_OUTPUT_DEFAULT_MEMLIMIT     (UINT64_C(1) << 25)
#define QUEUE_OUTPUT_MAX_PROCESSLIMIT     (UINT64_C(1) << 26)
#define QUEUE_OUTPUT_DEFAULT_PROCESSLIMIT (UINT64_C(1) << 27)
#define QUEUE_OUTPUT_MAX_STACKLIMIT       (UINT64_C(1) << 28)
#define QUEUE_OUTPUT_MAX_SWAPLIMIT        (UINT64_C(1) << 29)
#define QUEUE_OUTPUT_MAX_TASKLIMIT        (UINT64_C(1) << 30)
#define QUEUE_OUTPUT_MIN_TASKLIMIT        (UINT64_C(1) << 31)
#define QUEUE_OUTPUT_DEFAULT_TASKLIMIT    (UINT64_C(1) << 32)

/* LIM output fields wire bits */
#define LIM_OUTPUT_HOST_NAME   (UINT64_C(1) << 0)
#define LIM_OUTPUT_TYPE        (UINT64_C(1) << 1)
#define LIM_OUTPUT_MODEL       (UINT64_C(1) << 2)
#define LIM_OUTPUT_CPU_FACTOR  (UINT64_C(1) << 3)
#define LIM_OUTPUT_MAX_CPUS    (UINT64_C(1) << 4)
#define LIM_OUTPUT_MAX_MEM     (UINT64_C(1) << 5)
#define LIM_OUTPUT_MAX_SWAP    (UINT64_C(1) << 6)
#define LIM_OUTPUT_SERVER      (UINT64_C(1) << 7)
#define LIM_OUTPUT_RESOURCES   (UINT64_C(1) << 8)
#define LIM_OUTPUT_MAX_TMP     (UINT64_C(1) << 9)
#define LIM_OUTPUT_N_PROCS     (UINT64_C(1) << 10)
#define LIM_OUTPUT_WINDOWS     (UINT64_C(1) << 11)

#define LIM_OUTPUT_VALID_MASK \
    (LIM_OUTPUT_HOST_NAME | LIM_OUTPUT_TYPE | \
     LIM_OUTPUT_MODEL | LIM_OUTPUT_CPU_FACTOR | \
     LIM_OUTPUT_MAX_CPUS | LIM_OUTPUT_MAX_MEM | \
     LIM_OUTPUT_MAX_SWAP | LIM_OUTPUT_SERVER | \
     LIM_OUTPUT_RESOURCES | LIM_OUTPUT_MAX_TMP | \
     LIM_OUTPUT_N_PROCS | LIM_OUTPUT_WINDOWS)

#define OUTPUT_HOST_MEMBERS(X)                                                  \
    X(HOST_OUTPUT_HOST_NAME, string, host)                                      \
    X((HOST_OUTPUT_STATUS | HOST_OUTPUT_AVAILABLE_MEM |                         \
       HOST_OUTPUT_RESERVED_MEM | HOST_OUTPUT_TOTAL_MEM), int, hStatus)          \
    X(HOST_OUTPUT_JL_U, int, userJobLimit)                                      \
    X(HOST_OUTPUT_MAX, int, maxJobs)                                            \
    X(HOST_OUTPUT_NJOBS, int, numJobs)                                          \
    X(HOST_OUTPUT_RUN, int, numRUN)                                             \
    X(HOST_OUTPUT_SSUSP, int, numSSUSP)                                         \
    X(HOST_OUTPUT_USUSP, int, numUSUSP)                                         \
    X(HOST_OUTPUT_RSV, int, numRESERVE)                                         \
    X(HOST_OUTPUT_DISPATCH_WINDOW, string, windows)

#define OUTPUT_QUEUE_MEMBERS(X)                                                   \
    X(QUEUE_OUTPUT_QUEUE_NAME, string, queue)                                     \
    X(QUEUE_OUTPUT_DESCRIPTION, string, description)                              \
    X(QUEUE_OUTPUT_PRIORITY, int, priority)                                       \
    X(QUEUE_OUTPUT_STATUS, int, qStatus)                                          \
    X(QUEUE_OUTPUT_MAX, int, maxJobs)                                             \
    X(QUEUE_OUTPUT_JL_U, int, userJobLimit)                                       \
    X(QUEUE_OUTPUT_JL_P, float, procJobLimit)                                     \
    X(QUEUE_OUTPUT_JL_H, int, hostJobLimit)                                       \
    X(QUEUE_OUTPUT_NJOBS, int, numJobs)                                           \
    X(QUEUE_OUTPUT_PEND, int, numPEND)                                            \
    X(QUEUE_OUTPUT_RUN, int, numRUN)                                              \
    X((QUEUE_OUTPUT_SUSP | QUEUE_OUTPUT_SSUSP), int, numSSUSP)                    \
    X((QUEUE_OUTPUT_SUSP | QUEUE_OUTPUT_USUSP), int, numUSUSP)                    \
    X(QUEUE_OUTPUT_RSV, int, numRESERVE)                                          \
    X(QUEUE_OUTPUT_NICE, short, nice)                                             \
    X(QUEUE_OUTPUT_HOSTS, string, hostList)                                       \
    X(QUEUE_OUTPUT_RES_REQ, string, resReq)                                       \
    X(QUEUE_OUTPUT_MAX_CORELIMIT, int, rLimits[LSF_RLIMIT_CORE])                  \
    X(QUEUE_OUTPUT_MAX_CPULIMIT, int, rLimits[LSF_RLIMIT_CPU])                   \
    X(QUEUE_OUTPUT_DEFAULT_CPULIMIT, int, defLimits[LSF_RLIMIT_CPU])              \
    X(QUEUE_OUTPUT_MAX_DATALIMIT, int, rLimits[LSF_RLIMIT_DATA])                  \
    X(QUEUE_OUTPUT_DEFAULT_DATALIMIT, int, defLimits[LSF_RLIMIT_DATA])            \
    X(QUEUE_OUTPUT_MAX_FILELIMIT, int, rLimits[LSF_RLIMIT_FSIZE])                 \
    X(QUEUE_OUTPUT_MAX_MEMLIMIT, int, rLimits[LSF_RLIMIT_RSS])                    \
    X(QUEUE_OUTPUT_DEFAULT_MEMLIMIT, int, defLimits[LSF_RLIMIT_RSS])              \
    X(QUEUE_OUTPUT_MAX_PROCESSLIMIT, int, rLimits[LSF_RLIMIT_PROCESS])            \
    X(QUEUE_OUTPUT_DEFAULT_PROCESSLIMIT, int, defLimits[LSF_RLIMIT_PROCESS])      \
    X(QUEUE_OUTPUT_MAX_STACKLIMIT, int, rLimits[LSF_RLIMIT_STACK])                \
    X(QUEUE_OUTPUT_MAX_SWAPLIMIT, int, rLimits[LSF_RLIMIT_SWAP])                  \
    X(QUEUE_OUTPUT_MAX_TASKLIMIT, int, procLimit)                                 \
    X(QUEUE_OUTPUT_MIN_TASKLIMIT, int, minProcLimit)                              \
    X(QUEUE_OUTPUT_DEFAULT_TASKLIMIT, int, defProcLimit)

#endif /* OUTWIRE_H */

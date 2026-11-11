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

/*
 * Pure wire protocol bit definitions for custom output RPCs.
 * Independent of formatter and cJSON to ensure protocol layering cleanliness.
 */

/* Job output fields wire bits */
#define JOB_OUTPUT_USER        (1ULL << 0)
#define JOB_OUTPUT_STAT        (1ULL << 1)
#define JOB_OUTPUT_QUEUE       (1ULL << 2)
#define JOB_OUTPUT_FROM_HOST   (1ULL << 3)
#define JOB_OUTPUT_EXEC_HOST   (1ULL << 4)
#define JOB_OUTPUT_JOB_NAME    (1ULL << 5)
#define JOB_OUTPUT_SUBMIT_TIME (1ULL << 6)
#define JOB_OUTPUT_PROJ_NAME   (1ULL << 7)
#define JOB_OUTPUT_CPU_USED    (1ULL << 8)
#define JOB_OUTPUT_MEM         (1ULL << 9)
#define JOB_OUTPUT_SWAP        (1ULL << 10)
#define JOB_OUTPUT_PIDS        (1ULL << 11)
#define JOB_OUTPUT_START_TIME  (1ULL << 12)
#define JOB_OUTPUT_FINISH_TIME (1ULL << 13)
#define JOB_OUTPUT_EXIT_CODE   (1ULL << 14)
#define JOB_OUTPUT_REASONS     (1ULL << 15)
#define JOB_OUTPUT_MAX_MEM     (1ULL << 16)
#define JOB_OUTPUT_AVG_MEM     (1ULL << 17)

/* Host output fields wire bits */
#define HOST_OUTPUT_HOST_NAME       (1ULL << 0)
#define HOST_OUTPUT_STATUS          (1ULL << 1)
#define HOST_OUTPUT_JL_U            (1ULL << 2)
#define HOST_OUTPUT_MAX             (1ULL << 3)
#define HOST_OUTPUT_NJOBS           (1ULL << 4)
#define HOST_OUTPUT_RUN             (1ULL << 5)
#define HOST_OUTPUT_SSUSP           (1ULL << 6)
#define HOST_OUTPUT_USUSP           (1ULL << 7)
#define HOST_OUTPUT_RSV             (1ULL << 8)
#define HOST_OUTPUT_DISPATCH_WINDOW (1ULL << 9)
#define HOST_OUTPUT_AVAILABLE_MEM   (1ULL << 10)
#define HOST_OUTPUT_RESERVED_MEM    (1ULL << 11)
#define HOST_OUTPUT_TOTAL_MEM       (1ULL << 12)

/* Queue output fields wire bits */
#define QUEUE_OUTPUT_QUEUE_NAME           (1ULL << 0)
#define QUEUE_OUTPUT_DESCRIPTION          (1ULL << 1)
#define QUEUE_OUTPUT_PRIORITY             (1ULL << 2)
#define QUEUE_OUTPUT_STATUS               (1ULL << 3)
#define QUEUE_OUTPUT_MAX                  (1ULL << 4)
#define QUEUE_OUTPUT_JL_U                 (1ULL << 5)
#define QUEUE_OUTPUT_JL_P                 (1ULL << 6)
#define QUEUE_OUTPUT_JL_H                 (1ULL << 7)
#define QUEUE_OUTPUT_NJOBS                (1ULL << 8)
#define QUEUE_OUTPUT_PEND                 (1ULL << 9)
#define QUEUE_OUTPUT_RUN                  (1ULL << 10)
#define QUEUE_OUTPUT_SUSP                 (1ULL << 11)
#define QUEUE_OUTPUT_RSV                  (1ULL << 12)
#define QUEUE_OUTPUT_USUSP                (1ULL << 13)
#define QUEUE_OUTPUT_SSUSP                (1ULL << 14)
#define QUEUE_OUTPUT_NICE                 (1ULL << 15)
#define QUEUE_OUTPUT_HOSTS                (1ULL << 16)
#define QUEUE_OUTPUT_RES_REQ              (1ULL << 17)
#define QUEUE_OUTPUT_MAX_CORELIMIT        (1ULL << 18)
#define QUEUE_OUTPUT_MAX_CPULIMIT         (1ULL << 19)
#define QUEUE_OUTPUT_DEFAULT_CPULIMIT     (1ULL << 20)
#define QUEUE_OUTPUT_MAX_DATALIMIT        (1ULL << 21)
#define QUEUE_OUTPUT_DEFAULT_DATALIMIT    (1ULL << 22)
#define QUEUE_OUTPUT_MAX_FILELIMIT        (1ULL << 23)
#define QUEUE_OUTPUT_MAX_MEMLIMIT         (1ULL << 24)
#define QUEUE_OUTPUT_DEFAULT_MEMLIMIT     (1ULL << 25)
#define QUEUE_OUTPUT_MAX_PROCESSLIMIT     (1ULL << 26)
#define QUEUE_OUTPUT_DEFAULT_PROCESSLIMIT (1ULL << 27)
#define QUEUE_OUTPUT_MAX_STACKLIMIT       (1ULL << 28)
#define QUEUE_OUTPUT_MAX_SWAPLIMIT        (1ULL << 29)
#define QUEUE_OUTPUT_MAX_TASKLIMIT        (1ULL << 30)
#define QUEUE_OUTPUT_MIN_TASKLIMIT        (1ULL << 31)
#define QUEUE_OUTPUT_DEFAULT_TASKLIMIT    (1ULL << 32)

/* LIM output fields wire bits */
#define LIM_OUTPUT_HOST_NAME   (1u << 0)
#define LIM_OUTPUT_TYPE        (1u << 1)
#define LIM_OUTPUT_MODEL       (1u << 2)
#define LIM_OUTPUT_CPU_FACTOR  (1u << 3)
#define LIM_OUTPUT_MAX_CPUS    (1u << 4)
#define LIM_OUTPUT_MAX_MEM     (1u << 5)
#define LIM_OUTPUT_MAX_SWAP    (1u << 6)
#define LIM_OUTPUT_SERVER      (1u << 7)
#define LIM_OUTPUT_RESOURCES   (1u << 8)
#define LIM_OUTPUT_MAX_TMP     (1u << 9)
#define LIM_OUTPUT_N_DISKS     (1u << 10)
#define LIM_OUTPUT_WINDOWS     (1u << 11)

#define LIM_OUTPUT_VALID_MASK  0xfffu

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

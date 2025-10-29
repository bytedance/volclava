/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsbatch.h 397 2007-11-26 19:04:00Z mblack $
 * Copyright (C) 2007 Platform Computing Inc
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

#ifndef LSBATCH_H
#define LSBATCH_H
#include <lsf.h>

#define _PATH_NULL      "/dev/null"

#define  MAX_VERSION_LEN     12
#define  MAX_HPART_USERS     100
#define  MAX_GROUPS          150
#define  MAX_CHARLEN         20
#define  MAX_LSB_NAME_LEN    60
#define  MAX_CMD_DESC_LEN    256
#define  MAX_USER_EQUIVALENT 128
#define  DEFAULT_MSG_DESC    "no description"

#define HOST_STAT_OK         0x0
#define HOST_STAT_BUSY       0x01
#define HOST_STAT_WIND       0x02
#define HOST_STAT_DISABLED   0x04
#define HOST_STAT_LOCKED     0x08
#define HOST_STAT_FULL       0x10
#define HOST_STAT_UNREACH    0x20
#define HOST_STAT_UNAVAIL    0x40
#define HOST_STAT_NO_LIM     0x80
#define HOST_STAT_EXCLUSIVE  0x100
#define HOST_STAT_LOCKED_MASTER 0x200

#define LSB_HOST_OK(status)     (status == HOST_STAT_OK)

#define LSB_HOST_BUSY(status)     ((status & HOST_STAT_BUSY) != 0)

#define LSB_HOST_CLOSED(status)     ((status & (HOST_STAT_WIND | \
					HOST_STAT_DISABLED |     \
					HOST_STAT_LOCKED |       \
					HOST_STAT_LOCKED_MASTER | \
					HOST_STAT_FULL |         \
					HOST_STAT_NO_LIM)) != 0)

#define LSB_HOST_FULL(status)          ((status & HOST_STAT_FULL) != 0)

#define LSB_HOST_UNREACH(status)     ((status & HOST_STAT_UNREACH) != 0)

#define LSB_HOST_UNAVAIL(status)     ((status & HOST_STAT_UNAVAIL) != 0)

#define HOST_BUSY_NOT          0x000
#define HOST_BUSY_R15S         0x001
#define HOST_BUSY_R1M          0x002
#define HOST_BUSY_R15M         0x004
#define HOST_BUSY_UT           0x008
#define HOST_BUSY_PG           0x010
#define HOST_BUSY_IO           0x020
#define HOST_BUSY_LS           0x040
#define HOST_BUSY_IT           0x080
#define HOST_BUSY_TMP          0x100
#define HOST_BUSY_SWP          0x200
#define HOST_BUSY_MEM          0x400

#define LSB_ISBUSYON(status, index)  \
      (((status[(index)/INTEGER_BITS]) & (1 << (index)%INTEGER_BITS)) != 0)

#define QUEUE_STAT_OPEN         0x01
#define QUEUE_STAT_ACTIVE       0x02
#define QUEUE_STAT_RUN          0x04
#define QUEUE_STAT_NOPERM       0x08
#define QUEUE_STAT_DISC         0x10
#define QUEUE_STAT_RUNWIN_CLOSE 0x20

#define Q_ATTRIB_EXCLUSIVE        0x01
#define Q_ATTRIB_DEFAULT          0x02
#define Q_ATTRIB_ROUND_ROBIN      0x04
#define Q_ATTRIB_FS               0x08
#define Q_ATTRIB_BACKFILL         0x80
#define Q_ATTRIB_HOST_PREFER      0x100
#define Q_ATTRIB_NO_INTERACTIVE   0x800
#define Q_ATTRIB_ONLY_INTERACTIVE 0x1000
#define Q_ATTRIB_NO_HOST_TYPE     0x2000
#define Q_ATTRIB_IGNORE_DEADLINE  0x4000
#define Q_ATTRIB_CHKPNT           0x8000
#define Q_ATTRIB_RERUNNABLE       0x10000
#define Q_ATTRIB_ENQUE_INTERACTIVE_AHEAD 0x80000

#define MASTER_NULL           200
#define MASTER_RESIGN         201
#define MASTER_RECONFIG       202
#define MASTER_FATAL          203
#define MASTER_MEM            204
#define MASTER_CONF           205

#define JOB_STAT_NULL         0x00
#define JOB_STAT_PEND         0x01
#define JOB_STAT_PSUSP        0x02
#define JOB_STAT_RUN          0x04
#define JOB_STAT_SSUSP        0x08
#define JOB_STAT_USUSP        0x10
#define JOB_STAT_EXIT         0x20
#define JOB_STAT_DONE         0x40
#define JOB_STAT_PDONE        (0x80)
#define JOB_STAT_PERR         (0x100)
#define JOB_STAT_WAIT         (0x200)
#define JOB_STAT_UNKWN        0x10000

#define    EVENT_JOB_NEW          1
#define    EVENT_JOB_START        2
#define    EVENT_JOB_STATUS       3
#define    EVENT_JOB_SWITCH       4
#define    EVENT_JOB_MOVE         5
#define    EVENT_QUEUE_CTRL       6
#define    EVENT_HOST_CTRL        7
#define    EVENT_MBD_DIE          8
#define    EVENT_MBD_UNFULFILL    9
#define    EVENT_JOB_FINISH       10
#define    EVENT_LOAD_INDEX       11
#define    EVENT_CHKPNT           12
#define    EVENT_MIG              13
#define    EVENT_PRE_EXEC_START   14
#define    EVENT_MBD_START        15
#define    EVENT_JOB_MODIFY       16
#define    EVENT_JOB_SIGNAL       17
#define    EVENT_JOB_EXECUTE      18
#define    EVENT_JOB_MSG          19
#define    EVENT_JOB_MSG_ACK      20
#define    EVENT_JOB_REQUEUE      21
#define    EVENT_JOB_SIGACT       22

#define    EVENT_SBD_JOB_STATUS   23
#define    EVENT_JOB_START_ACCEPT 24
#define    EVENT_JOB_CLEAN        25
#define    EVENT_JOB_FORCE        26
#define    EVENT_LOG_SWITCH       27
#define    EVENT_JOB_MODIFY2      28
#define    EVENT_JOB_ATTR_SET     29
#define    EVENT_UNUSED_30        30
#define    EVENT_UNUSED_31        31
#define    EVENT_UNUSED_32        32


#define PEND_JOB_REASON        0
#define PEND_JOB_NEW           1
#define PEND_JOB_START_TIME    2
#define PEND_JOB_DEPEND        3
#define PEND_JOB_DEP_INVALID   4
#define PEND_JOB_MIG           5
#define PEND_JOB_PRE_EXEC      6
#define PEND_JOB_NO_FILE       7
#define PEND_JOB_ENV           8
#define PEND_JOB_PATHS         9
#define PEND_JOB_OPEN_FILES    10
#define PEND_JOB_EXEC_INIT     11
#define PEND_JOB_RESTART_FILE  12
#define PEND_JOB_DELAY_SCHED   13
#define PEND_JOB_SWITCH        14
#define PEND_JOB_DEP_REJECT    15
#define PEND_JOB_NO_PASSWD     17
#define PEND_JOB_MODIFY        19
#define PEND_JOB_REQUEUED      23
#define PEND_SYS_UNABLE        35
#define PEND_JOB_ARRAY_JLIMIT  38
#define PEND_CHKPNT_DIR        39

#define PEND_QUE_INACT             301
#define PEND_QUE_WINDOW            302
#define PEND_QUE_JOB_LIMIT         303
#define PEND_QUE_USR_JLIMIT        304
#define PEND_QUE_USR_PJLIMIT       305
#define PEND_QUE_PRE_FAIL          306
#define PEND_SYS_NOT_READY         310
#define PEND_SBD_JOB_REQUEUE       311
#define PEND_JOB_SPREAD_TASK       312
#define PEND_QUE_SPREAD_TASK       313
#define PEND_QUE_PJOB_LIMIT        314
#define PEND_QUE_WINDOW_WILL_CLOSE 315
#define PEND_QUE_PROCLIMIT	   316

#define PEND_USER_JOB_LIMIT    601
#define PEND_UGRP_JOB_LIMIT    602
#define PEND_USER_PJOB_LIMIT   603
#define PEND_UGRP_PJOB_LIMIT   604
#define PEND_USER_RESUME       605
#define PEND_USER_STOP         607
#define PEND_NO_MAPPING        608
#define PEND_RMT_PERMISSION    609
#define PEND_ADMIN_STOP        610


#define PEND_HOST_RES_REQ      1001
#define PEND_HOST_NONEXCLUSIVE 1002
#define PEND_HOST_JOB_SSUSP    1003
#define PEND_SBD_GETPID        1005
#define PEND_SBD_LOCK          1006
#define PEND_SBD_ZOMBIE        1007
#define PEND_SBD_ROOT          1008
#define PEND_HOST_WIN_WILL_CLOSE 1009
#define PEND_HOST_MISS_DEADLINE  1010
#define PEND_FIRST_HOST_INELIGIBLE 1011
#define PEND_HOST_DISABLED     1301
#define PEND_HOST_LOCKED       1302
#define PEND_HOST_LESS_SLOTS   1303
#define PEND_HOST_WINDOW       1304
#define PEND_HOST_JOB_LIMIT    1305
#define PEND_QUE_PROC_JLIMIT   1306
#define PEND_QUE_HOST_JLIMIT   1307
#define PEND_USER_PROC_JLIMIT  1308
#define PEND_HOST_USR_JLIMIT   1309
#define PEND_HOST_QUE_MEMB     1310
#define PEND_HOST_USR_SPEC     1311
#define PEND_HOST_PART_USER    1312
#define PEND_HOST_NO_USER      1313
#define PEND_HOST_ACCPT_ONE    1314
#define PEND_LOAD_UNAVAIL      1315
#define PEND_HOST_NO_LIM       1316
#define PEND_HOST_QUE_RESREQ   1318
#define PEND_HOST_SCHED_TYPE   1319
#define PEND_JOB_NO_SPAN       1320
#define PEND_QUE_NO_SPAN       1321
#define PEND_HOST_EXCLUSIVE    1322
#define PEND_UGRP_PROC_JLIMIT  1324
#define PEND_BAD_HOST          1325
#define PEND_QUEUE_HOST        1326
#define PEND_HOST_LOCKED_MASTER 1327

#define PEND_SBD_UNREACH       1601
#define PEND_SBD_JOB_QUOTA     1602
#define PEND_JOB_START_FAIL    1603
#define PEND_JOB_START_UNKNWN  1604
#define PEND_SBD_NO_MEM        1605
#define PEND_SBD_NO_PROCESS    1606
#define PEND_SBD_SOCKETPAIR    1607
#define PEND_SBD_JOB_ACCEPT    1608

#define PEND_HOST_LOAD         2001

#define PEND_HOST_QUE_RUSAGE  2301

#define PEND_HOST_JOB_RUSAGE 2601

#define PEND_MAX_REASONS 2900


#define SUSP_USER_REASON      0x00000000
#define SUSP_USER_RESUME      0x00000001
#define SUSP_USER_STOP        0x00000002

#define SUSP_QUEUE_REASON     0x00000004
#define SUSP_QUEUE_WINDOW     0x00000008
#define SUSP_HOST_LOCK        0x00000020
#define SUSP_LOAD_REASON      0x00000040
#define SUSP_QUE_STOP_COND    0x00000200
#define SUSP_QUE_RESUME_COND  0x00000400
#define SUSP_PG_IT            0x00000800
#define SUSP_REASON_RESET     0x00001000
#define SUSP_LOAD_UNAVAIL     0x00002000
#define SUSP_ADMIN_STOP       0x00004000
#define SUSP_RES_RESERVE      0x00008000
#define SUSP_MBD_LOCK         0x00010000
#define SUSP_RES_LIMIT        0x00020000
#define SUB_REASON_RUNLIMIT     0x00000001
#define SUB_REASON_DEADLINE     0x00000002
#define SUB_REASON_PROCESSLIMIT 0x00000004
#define SUB_REASON_CPULIMIT     0x00000008
#define SUB_REASON_MEMLIMIT     0x00000010
#define SUSP_SBD_STARTUP        0x00040000
#define SUSP_HOST_LOCK_MASTER   0x00080000
#define EXIT_NORMAL         0x00000000
#define EXIT_RESTART        0x00000001
#define EXIT_ZOMBIE         0x00000002
#define FINISH_PEND         0x00000004
#define EXIT_KILL_ZOMBIE    0x00000008
#define EXIT_ZOMBIE_JOB     0x00000010
#define EXIT_RERUN          0x00000020
#define EXIT_NO_MAPPING     0x00000040
#define EXIT_INIT_ENVIRON   0x00000100
#define EXIT_PRE_EXEC       0x00000200
#define EXIT_REQUEUE        0x00000400
#define EXIT_REMOVE         0x00000800
#define EXIT_VALUE_REQUEUE  0x00001000

#define LSB_MODE_BATCH    0x1

#define    LSBE_NO_ERROR      00
#define    LSBE_NO_JOB        01
#define    LSBE_NOT_STARTED   02
#define    LSBE_JOB_STARTED   03
#define    LSBE_JOB_FINISH    04
#define    LSBE_STOP_JOB      05
#define    LSBE_DEPEND_SYNTAX  6
#define    LSBE_EXCLUSIVE      7
#define    LSBE_ROOT           8
#define    LSBE_MIGRATION      9
#define    LSBE_J_UNCHKPNTABLE 10
#define    LSBE_NO_OUTPUT      11
#define    LSBE_NO_JOBID       12
#define    LSBE_ONLY_INTERACTIVE 13
#define    LSBE_NO_INTERACTIVE   14

#define    LSBE_NO_USER       15
#define    LSBE_BAD_USER      16
#define    LSBE_PERMISSION    17
#define    LSBE_BAD_QUEUE     18
#define    LSBE_QUEUE_NAME    19
#define    LSBE_QUEUE_CLOSED  20
#define    LSBE_QUEUE_WINDOW  21
#define    LSBE_QUEUE_USE     22
#define    LSBE_BAD_HOST      23
#define    LSBE_PROC_NUM      24
#define    LSBE_RESERVE1      25
#define    LSBE_RESERVE2      26
#define    LSBE_NO_GROUP      27
#define    LSBE_BAD_GROUP     28
#define    LSBE_QUEUE_HOST    29
#define    LSBE_UJOB_LIMIT    30
#define    LSBE_NO_HOST       31

#define    LSBE_BAD_CHKLOG    32
#define    LSBE_PJOB_LIMIT    33
#define    LSBE_NOLSF_HOST    34

#define    LSBE_BAD_ARG       35
#define    LSBE_BAD_TIME      36
#define    LSBE_START_TIME    37
#define    LSBE_BAD_LIMIT     38
#define    LSBE_OVER_LIMIT    39
#define    LSBE_BAD_CMD       40
#define    LSBE_BAD_SIGNAL    41
#define    LSBE_BAD_JOB       42
#define    LSBE_QJOB_LIMIT    43

#define    LSBE_UNKNOWN_EVENT 44
#define    LSBE_EVENT_FORMAT  45
#define    LSBE_EOF           46

#define    LSBE_MBATCHD       47
#define    LSBE_SBATCHD       48
#define    LSBE_LSBLIB        49
#define    LSBE_LSLIB         50
#define    LSBE_SYS_CALL      51
#define    LSBE_NO_MEM        52
#define    LSBE_SERVICE       53
#define    LSBE_NO_ENV        54
#define    LSBE_CHKPNT_CALL   55
#define    LSBE_NO_FORK       56

#define    LSBE_PROTOCOL      57
#define    LSBE_XDR           58
#define    LSBE_PORT          59
#define    LSBE_TIME_OUT      60
#define    LSBE_CONN_TIMEOUT  61
#define    LSBE_CONN_REFUSED  62
#define    LSBE_CONN_EXIST    63
#define    LSBE_CONN_NONEXIST 64
#define    LSBE_SBD_UNREACH   65
#define    LSBE_OP_RETRY      66
#define    LSBE_USER_JLIMIT   67

#define    LSBE_JOB_MODIFY       68
#define    LSBE_JOB_MODIFY_ONCE  69

#define    LSBE_J_UNREPETITIVE   70
#define    LSBE_BAD_CLUSTER      71

#define    LSBE_JOB_MODIFY_USED  72

#define    LSBE_HJOB_LIMIT       73

#define    LSBE_NO_JOBMSG        74

#define    LSBE_BAD_RESREQ       75

#define    LSBE_NO_ENOUGH_HOST   76

#define    LSBE_CONF_FATAL       77
#define    LSBE_CONF_WARNING     78


#define    LSBE_NO_RESOURCE        79
#define    LSBE_BAD_RESOURCE       80
#define    LSBE_INTERACTIVE_RERUN  81
#define    LSBE_PTY_INFILE         82
#define    LSBE_BAD_SUBMISSION_HOST  83
#define    LSBE_LOCK_JOB           84
#define    LSBE_UGROUP_MEMBER      85
#define    LSBE_OVER_RUSAGE        86
#define    LSBE_BAD_HOST_SPEC      87
#define    LSBE_BAD_UGROUP         88
#define    LSBE_ESUB_ABORT         89
#define    LSBE_EXCEPT_ACTION      90
#define    LSBE_JOB_DEP            91
#define    LSBE_JGRP_NULL           92
#define    LSBE_JGRP_BAD            93
#define    LSBE_JOB_ARRAY           94
#define    LSBE_JOB_SUSP            95
#define    LSBE_JOB_FORW            96
#define    LSBE_BAD_IDX             97
#define    LSBE_BIG_IDX             98
#define    LSBE_ARRAY_NULL          99
#define    LSBE_JOB_EXIST           100
#define    LSBE_JOB_ELEMENT         101
#define    LSBE_BAD_JOBID           102
#define    LSBE_MOD_JOB_NAME        103

#define    LSBE_PREMATURE           104

#define    LSBE_BAD_PROJECT_GROUP   105

#define    LSBE_NO_HOST_GROUP       106
#define    LSBE_NO_USER_GROUP       107
#define    LSBE_INDEX_FORMAT        108

#define    LSBE_SP_SRC_NOT_SEEN     109
#define    LSBE_SP_FAILED_HOSTS_LIM 110
#define    LSBE_SP_COPY_FAILED      111
#define    LSBE_SP_FORK_FAILED      112
#define    LSBE_SP_CHILD_DIES       113
#define    LSBE_SP_CHILD_FAILED     114
#define    LSBE_SP_FIND_HOST_FAILED 115
#define    LSBE_SP_SPOOLDIR_FAILED  116
#define    LSBE_SP_DELETE_FAILED    117

#define    LSBE_BAD_USER_PRIORITY   118
#define    LSBE_NO_JOB_PRIORITY     119
#define    LSBE_JOB_REQUEUED        120

#define    LSBE_MULTI_FIRST_HOST    121
#define    LSBE_HG_FIRST_HOST       122
#define    LSBE_HP_FIRST_HOST       123
#define    LSBE_OTHERS_FIRST_HOST   124

#define    LSBE_PROC_LESS     	    125
#define    LSBE_MOD_MIX_OPTS        126
#define    LSBE_MOD_CPULIMIT        127
#define    LSBE_MOD_MEMLIMIT        128
#define    LSBE_MOD_ERRFILE         129
#define    LSBE_LOCKED_MASTER       130
#define    LSBE_DEP_ARRAY_SIZE      131
#define    LSBE_JOB_MAX_PEND        132
#define    LSBE_SLOTS_MAX_PEND      133
#define    LSBE_BAD_RLIMIT          134
#define    LSBE_NUM_ERR             134

#define  SUB_JOB_NAME       0x01
#define  SUB_QUEUE          0x02
#define  SUB_HOST           0x04
#define  SUB_IN_FILE        0x08
#define  SUB_OUT_FILE       0x10
#define  SUB_ERR_FILE       0x20
#define  SUB_EXCLUSIVE      0x40
#define  SUB_NOTIFY_END     0x80
#define  SUB_NOTIFY_BEGIN   0x100
#define  SUB_USER_GROUP	    0x200
#define  SUB_CHKPNT_PERIOD  0x400
#define  SUB_CHKPNT_DIR     0x800
#define  SUB_CHKPNTABLE     SUB_CHKPNT_DIR
#define  SUB_RESTART_FORCE  0x1000
#define  SUB_RESTART        0x2000
#define  SUB_RERUNNABLE     0x4000
#define  SUB_WINDOW_SIG     0x8000
#define  SUB_HOST_SPEC      0x10000
#define  SUB_DEPEND_COND    0x20000
#define  SUB_RES_REQ        0x40000
#define  SUB_OTHER_FILES    0x80000
#define  SUB_PRE_EXEC	    0x100000
#define  SUB_LOGIN_SHELL    0x200000
#define  SUB_MAIL_USER 	    0x400000
#define  SUB_MODIFY         0x800000
#define  SUB_MODIFY_ONCE    0x1000000
#define  SUB_PROJECT_NAME   0x2000000
#define  SUB_INTERACTIVE    0x4000000
#define  SUB_PTY            0x8000000
#define  SUB_PTY_SHELL      0x10000000
#define  SUB_PACK           0x20000000
#define  SUB_POST_EXEC      0x40000000

#define  SUB2_HOLD          0x01
#define  SUB2_MODIFY_CMD    0x02
#define  SUB2_BSUB_BLOCK    0x04
#define  SUB2_HOST_NT       0x08
#define  SUB2_HOST_UX       0x10
#define  SUB2_QUEUE_CHKPNT  0x20
#define  SUB2_QUEUE_RERUNNABLE  0x40
#define  SUB2_IN_FILE_SPOOL 0x80
#define  SUB2_JOB_CMD_SPOOL 0x100
#define  SUB2_JOB_PRIORITY  0x200
#define  SUB2_USE_DEF_PROCLIMIT  0x400
#define  SUB2_MODIFY_RUN_JOB 0x800
#define  SUB2_MODIFY_PEND_JOB 0x1000

#define  LOST_AND_FOUND  "lost_and_found"

#define  DELETE_NUMBER     -2
#define  DEL_NUMPRO        INFINIT_INT
#define  DEFAULT_NUMPRO    INFINIT_INT -1

struct xFile {
    char subFn[MAXFILENAMELEN];
    char execFn[MAXFILENAMELEN];
    int options;
#define  XF_OP_SUB2EXEC         0x1
#define  XF_OP_EXEC2SUB         0x2
#define  XF_OP_SUB2EXEC_APPEND  0x4
#define  XF_OP_EXEC2SUB_APPEND  0x8
#define  XF_OP_URL_SOURCE       0x10
};

struct submit {
    int     options;
    int     options2;
    char    *jobName;
    char    *packFile;
    char    *queue;
    int     numAskedHosts;
    char    **askedHosts;
    char    *resReq;
    int     rLimits[LSF_RLIM_NLIMITS];
    char    *hostSpec;
    int     numProcessors;
    char    *dependCond;
    time_t  beginTime;
    time_t  termTime;
    int     sigValue;
    char    *inFile;
    char    *outFile;
    char    *errFile;
    char    *command;
    char    *newCommand;
    time_t  chkpntPeriod;
    char    *chkpntDir;
    int     nxf;
    struct xFile *xf;
    char    *preExecCmd;
    char    *postExecCmd;
    char    *mailUser;
    int    delOptions;
    int    delOptions2;
    char   *projectName;
    int    maxNumProcessors;
    char   *loginShell;
    int    userPriority;
};

struct submitReply {
     char    *queue;
     LS_LONG_INT  badJobId;
     char    *badJobName;
     char    *pendLimitReason;
     int     badReqIndx;
     int     subTryInterval;
};

struct submig {
    LS_LONG_INT jobId;
    int options;
    int numAskedHosts;
    char **askedHosts;
};


#define LSB_CHKPERIOD_NOCHNG -1

#define LSB_CHKPNT_KILL  0x1
#define LSB_CHKPNT_FORCE 0x2
#define LSB_CHKPNT_COPY  0x3
#define LSB_CHKPNT_MIG   0x4
#define LSB_CHKPNT_STOP  0x8
#define LSB_KILL_REQUEUE 0x10

#define ALL_USERS       "all"
#define ALL_JOB         0x0001
#define DONE_JOB        0x0002
#define PEND_JOB        0x0004
#define SUSP_JOB        0x0008
#define CUR_JOB         0x0010
#define LAST_JOB        0x0020
#define RUN_JOB         0x0040
#define JOBID_ONLY      0x0080
#define HOST_NAME       0x0100
#define NO_PEND_REASONS 0x0200
#define JGRP_ARRAY_INFO 0x1000
#define JOBID_ONLY_ALL  0x02000
#define ZOMBIE_JOB      0x04000

#define    JGRP_NODE_JOB	1
#define    JGRP_NODE_GROUP	2
#define    JGRP_NODE_ARRAY	3

#define LSB_MAX_ARRAY_JOBID	0x0FFFFFFFF
#define LSB_MAX_ARRAY_IDX	0x0FFFF
#define LSB_MAX_SEDJOB_RUNID    (0x0F)
#define LSB_JOBID(array_jobId, array_idx)  \
                 (((LS_UNS_LONG_INT)array_idx << 32) | array_jobId)
#define LSB_ARRAY_IDX(jobId) \
        (((jobId) == -1) ? (0) : (int)(((LS_UNS_LONG_INT)jobId >> 32) \
                                                    & LSB_MAX_ARRAY_IDX))
#define LSB_ARRAY_JOBID(jobId)\
                 (((jobId) == -1) ? (-1) : (int)(jobId & LSB_MAX_ARRAY_JOBID))


#define    JGRP_ACTIVE        1
#define    JGRP_UNDEFINED     -1

#define     JGRP_COUNT_NJOBS   0
#define     JGRP_COUNT_PEND    1
#define     JGRP_COUNT_NPSUSP  2
#define     JGRP_COUNT_NRUN    3
#define     JGRP_COUNT_NSSUSP  4
#define     JGRP_COUNT_NUSUSP  5
#define     JGRP_COUNT_NEXIT   6
#define     JGRP_COUNT_NDONE   7

#define    NUM_JGRP_COUNTERS 8

struct jobAttrInfoEnt {
    LS_LONG_INT jobId;
    u_short   port;
    char      hostname[MAXHOSTNAMELEN];
};

struct jobAttrSetLog {
    int       jobId;
    int       idx;
    int       uid;
    int       port;
    char      *hostname;
};

struct jobInfoHead {
    int   numJobs;
    LS_LONG_INT *jobIds;
    int   numHosts;
    char  **hostNames;
};

struct jobInfoEnt {
    LS_LONG_INT jobId;
    char    *user;
    int     status;
    int     *reasonTb;
    int     numReasons;
    int     reasons;
    int     subreasons;
    int     jobPid;
    time_t  submitTime;
    time_t  reserveTime;
    time_t  startTime;
    time_t  predictedStartTime;
    time_t  endTime;
    float   cpuTime;
    int     umask;
    char    *cwd;
    char    *subHomeDir;
    char    *fromHost;
    char    **exHosts;
    int     numExHosts;
    float   cpuFactor;
    int     nIdx;
    float   *loadSched;
    float   *loadStop;
    struct  submit submit;
    int     exitStatus;
    int     execUid;
    char    *execHome;
    char    *execCwd;
    char    *execUsername;
    time_t  jRusageUpdateTime;
    struct  jRusage runRusage;
    int     jType;
    char    *parentGroup;
    char    *jName;
    int     counter[NUM_JGRP_COUNTERS];
    u_short port;
    int     jobPriority;
    char    *chargedSAAP;
};

struct userInfoEnt {
    char   *user;
    float  procJobLimit;
    int    maxJobs;
    int    maxPendJobs;
    int    maxPendSlots;
    int    numStartJobs;
    int    numJobs;
    int    numPEND;// pend slots
    int    numPENDJobs;
    int    numRUN;
    int    numSSUSP;
    int    numUSUSP;
    int    numRESERVE;
};

#define ALL_QUEUE       0x01
#define DFT_QUEUE       0x02
#define CHECK_HOST      0x80
#define CHECK_USER      0x100
#define SORT_HOST       0x200

#define LSB_SIG_NUM               23

struct fsFactors {
    float cpuTimeFactor;      
    float runTimeFactor;      
    float runJobFactor;       
    float histHours;          
};

struct queueInfoEnt {
    char   *queue;
    char   *description;
    int    priority;
    short  nice;
    char   *userList;
    char   *hostList;
    int    nIdx;
    float  *loadSched;
    float  *loadStop;
    int    userJobLimit;
    float  procJobLimit;
    char   *windows;
    int    rLimits[LSF_RLIM_NLIMITS];
    char   *hostSpec;
    int    qAttrib;
    int    qStatus;
    int    maxJobs;
    int    numJobs;
    int    numPEND;
    int    numRUN;
    int    numSSUSP;
    int    numUSUSP;
    int    mig;
    int    schedDelay;
    int    acceptIntvl;
    char   *windowsD;
    char   *defaultHostSpec;
    int    procLimit;
    char   *admins;
    char   *preCmd;
    char   *postCmd;
    char   *prepostUsername;
    char   *requeueEValues;
    int    hostJobLimit;
    char   *resReq;
    int    numRESERVE;
    int    slotHoldTime;
    char   *resumeCond;
    char   *stopCond;
    char   *jobStarter;
    char   *suspendActCmd;
    char   *resumeActCmd;
    char   *terminateActCmd;
    int    sigMap[LSB_SIG_NUM];
    char   *chkpntDir;
    int    chkpntPeriod;
    int    defLimits[LSF_RLIM_NLIMITS];
    int    minProcLimit;
    int    defProcLimit;
    char   *userShares;
    struct shareAcctInfoEnt *shareAcctTree;
    struct fsFactors fsFactors;
    char   *actionComment;
};

struct shareAcctInfoEnt {
    char  *name;
    int    nChildShareAcct;
    struct shareAcctInfoEnt *childShareAccts; 
    int    share;
    float  priority;
    int    numStartJobs;
    float  cpuTime;
    float  runTime;
};


#define ACT_NO              0
#define ACT_START           1
#define ACT_DONE            3
#define ACT_FAIL            4


struct hostInfoEnt {
    char   *host;
    int    hStatus;
    int    *busySched;
    int    *busyStop;
    float  cpuFactor;
    int    nIdx;
    float *load;
    float  *loadSched;
    float  *loadStop;
    char   *windows;
    int    userJobLimit;
    int    maxJobs;
    int    numJobs;
    int    numRUN;
    int    numSSUSP;
    int    numUSUSP;
    int    mig;
    int    attr;
#define H_ATTR_CHKPNTABLE  0x1
    float *realLoad;
    int   numRESERVE;
    int   chkSig;
    char  *actionComment;
};

#define DEF_MAX_JOBID   999999
#define MAX_JOBID_LOW   999999
#define MAX_JOBID_HIGH 9999999
#define DEF_SUB_TRY_INTERVAL 60
#define DEF_CPU_TIME_FACTOR 0.7
#define DEF_RUN_TIME_FACTOR 0.7
#define DEF_RUN_JOB_FACTOR  3.0
#define DEF_HIST_HOURS      5

struct parameterInfo {
    char *defaultQueues;
    char *defaultHostSpec;
    int  mbatchdInterval;
    int  sbatchdInterval;
    int  jobAcceptInterval;
    int  maxDispRetries;
    int  maxSbdRetries;
    int  cleanPeriod;
    int  maxNumJobs;
    int  pgSuspendIt;
    char *defaultProject;
    int  retryIntvl;
    int  rusageUpdateRate;
    int  rusageUpdatePercent;
    int  condCheckTime;
    int  maxSbdConnections;
    int  maxSchedStay;
    int  freshPeriod;
    int  maxJobArraySize;
    int  jobTerminateInterval;
    int  disableUAcctMap;
    int  jobRunTimes;
    int  jobDepLastSub;
    char *pjobSpoolDir;
    int  maxUserPriority;
    int  jobPriorityValue;
    int  jobPriorityTime;
    int  sharedResourceUpdFactor;
    int  scheRawLoad;
    int  preExecDelay;
    int  slotResourceReserve;
    int	 maxJobId;
    int  maxAcctArchiveNum;
    int  acctArchiveInDays;
    int  acctArchiveInSize;
    int  resourcePerTask;
    int  subTryInterval;
    int  maxPendJobs;
    int  maxPendSlots;
    int  defaultLimitIgnoreUserGroup;
    float cpuTimeFactor;
    float runTimeFactor;
    float runJobFactor;
    float histHours;
};


struct loadInfoEnt {
    char   *hostName;
    int    status;
    float  *load;
};

#define USER_GRP          0x1
#define HOST_GRP          0x2
#define GRP_RECURSIVE     0x8
#define GRP_ALL           0x10
#define GRP_SHARES        0x40
struct userShares {
    char  *name;
    int    share;
};

struct groupInfoEnt {
    char *group;
    char *memberList;
#define GRP_IS_UGRP            0x01 /**< Group is a user group. */
#define GRP_IS_HGRP            0x02 /**< Group is a host group. */
    int  options;
    int numUserShares;
    struct userShares *userShares;
};

struct runJobRequest {
    LS_LONG_INT jobId;
    int numHosts;
    char **hostname;
#define RUNJOB_OPT_NORMAL     0x01
#define RUNJOB_OPT_NOSTOP     0x02
#define RUNJOB_OPT_PENDONLY   0x04
#define RUNJOB_OPT_FROM_BEGIN 0x08
    int     options;
};

#define REQUEUE_DONE   0x1
#define REQUEUE_EXIT   0x2 
#define REQUEUE_RUN    0x4

struct jobrequeue {
    LS_LONG_INT      jobId;
    int              status;
    int              options;
};

#define    TO_TOP            1
#define    TO_BOTTOM         2

#define    QUEUE_OPEN        1
#define    QUEUE_CLOSED      2
#define    QUEUE_ACTIVATE    3
#define    QUEUE_INACTIVATE  4

#define    HOST_OPEN         1
#define    HOST_CLOSE        2
#define    HOST_REBOOT       3
#define    HOST_SHUTDOWN     4

#define    MBD_RESTART       0
#define    MBD_RECONFIG      1
#define    MBD_CKCONFIG      2

struct logSwitchLog {
    int lastJobId;
};

struct jobNewLog {
    int    jobId;
    int    userId;
    char   userName[MAX_LSB_NAME_LEN];
    int    options;
    int    options2;
    int    numProcessors;
    time_t submitTime;
    time_t beginTime;
    time_t termTime;
    int    sigValue;
    int    chkpntPeriod;
    int    restartPid;
    int    rLimits[LSF_RLIM_NLIMITS];
    char   hostSpec[MAXHOSTNAMELEN];
    float  hostFactor;
    int    umask;
    char   queue[MAX_LSB_NAME_LEN];
    char   *resReq;
    char   fromHost[MAXHOSTNAMELEN];
    char   cwd[MAXFILENAMELEN];
    char   chkpntDir[MAXFILENAMELEN];
    char   inFile[MAXFILENAMELEN];
    char   outFile[MAXFILENAMELEN];
    char   errFile[MAXFILENAMELEN];
    char   inFileSpool[MAXFILENAMELEN];
    char   commandSpool[MAXFILENAMELEN];
    char   jobSpoolDir[MAXPATHLEN];
    char   subHomeDir[MAXFILENAMELEN];
    char   jobFile[MAXFILENAMELEN];
    int    numAskedHosts;
    char   **askedHosts;
    char   *dependCond;
    char   jobName[MAXLINELEN];
    char   command[MAXLINELEN];
    int    nxf;
    struct xFile *xf;
    char   *preExecCmd;
    char   *postExecCmd;
    char   *mailUser;
    char   *projectName;
    int    niosPort;
    int    maxNumProcessors;
    char   *schedHostType;
    char   *loginShell;
    int    idx;
    int    userPriority;
};

struct jobModLog {

    char    *jobIdStr;
    int     options;
    int     options2;
    int     delOptions;
    int     delOptions2;

    int     userId;
    char    *userName;

    int     submitTime;
    int     umask;
    int     numProcessors;
    int     beginTime;
    int     termTime;
    int     sigValue;
    int     restartPid;

    char    *jobName;
    char    *queue;

    int     numAskedHosts;
    char    **askedHosts;

    char    *resReq;
    int     rLimits[LSF_RLIM_NLIMITS];
    char    *hostSpec;

    char    *dependCond;
    char    *subHomeDir;
    char    *inFile;
    char    *outFile;
    char    *errFile;
    char    *command;
    char    *inFileSpool;
    char    *commandSpool;
    int     chkpntPeriod;
    char    *chkpntDir;
    int     nxf;
    struct  xFile *xf;

    char    *jobFile;
    char    *fromHost;
    char    *cwd;

    char    *preExecCmd;
    char    *postExecCmd;
    char    *mailUser;
    char    *projectName;

    int     niosPort;
    int     maxNumProcessors;

    char    *loginShell;
    char    *schedHostType;
    int     userPriority;
};


struct jobStartLog {
    int jobId;
    int    jStatus;
    int    jobPid;
    int    jobPGid;
    float  hostFactor;
    int    numExHosts;
    char   **execHosts;
    char   *queuePreCmd;
    char   *queuePostCmd;
    int	   jFlags;
    int    idx;
};

struct jobStartAcceptLog {
    int    jobId;
    int    jobPid;
    int    jobPGid;
    int    idx;
};


struct jobExecuteLog {
    int    jobId;
    int    execUid;
    char   *execHome;
    char   *execCwd;
    int    jobPGid;
    char   *execUsername;
    int    jobPid;
    int    idx;
};


struct jobStatusLog {
    int    jobId;
    int    jStatus;
    int    reason;
    int    subreasons;
    float  cpuTime;
    time_t endTime;
    int    ru;
    struct lsfRusage lsfRusage;
    int   jFlags;
    int   exitStatus;
    int    idx;
};


struct sbdJobStatusLog {
    int    jobId;
    int    jStatus;
    int    reasons;
    int    subreasons;
    int    actPid;
    int    actValue;
    time_t actPeriod;
    int    actFlags;
    int    actStatus;
    int    actReasons;
    int    actSubReasons;
    int    idx;
};

struct jobSwitchLog {
    int    userId;
    int jobId;
    char   queue[MAX_LSB_NAME_LEN];
    int    idx;
    char   userName[MAX_LSB_NAME_LEN];
};

struct jobMoveLog {
    int    userId;
    int    jobId;
    int    position;
    int    base;
    int    idx;
    char   userName[MAX_LSB_NAME_LEN];
};

struct chkpntLog {
    int jobId;
    time_t period;
    int pid;
    int ok;
    int flags;
    int    idx;
};

struct jobRequeueLog {
    int jobId;
    int    idx;
};

struct jobCleanLog {
    int jobId;
    int    idx;
};

struct sigactLog {
    int jobId;
    time_t period;
    int pid;
    int jStatus;
    int reasons;
    int flags;
    char *signalSymbol;
    int actStatus;
    int    idx;
};

struct migLog {
    int jobId;
    int numAskedHosts;
    char **askedHosts;
    int userId;
    int    idx;
    char userName[MAX_LSB_NAME_LEN];
};

struct signalLog {
    int userId;
    int jobId;
    char *signalSymbol;
    int runCount;
    int    idx;
    char userName[MAX_LSB_NAME_LEN];
};
struct queueCtrlLog {
    int    opCode;
    char   queue[MAX_LSB_NAME_LEN];
    int    userId;
    char   userName[MAX_LSB_NAME_LEN];
    char   msg[MAXLINELEN];
};

struct newDebugLog {
    int opCode;
    int level;
    int logclass;
    int turnOff;
    char logFileName[MAXLSFNAMELEN];
    int userId;
 };

struct hostCtrlLog {
    int    opCode;
    char   host[MAXHOSTNAMELEN];
    int    userId;
    char   userName[MAX_LSB_NAME_LEN];
    char   msg[MAXLINELEN];
};

struct mbdStartLog {
    char   master[MAXHOSTNAMELEN];
    char   cluster[MAXLSFNAMELEN];
    int    numHosts;
    int    numQueues;
};

struct mbdDieLog {
    char   master[MAXHOSTNAMELEN];
    int    numRemoveJobs;
    int    exitCode;
    char   msg[MAXLINELEN];
};

struct unfulfillLog {
    int    jobId;
    int    notSwitched;
    int    sig;
    int    sig1;
    int    sig1Flags;
    time_t chkPeriod;
    int    notModified;
    int    idx;
};

struct jobFinishLog {
    int    jobId;
    int    userId;
    char   userName[MAX_LSB_NAME_LEN];
    int    options;
    int    numProcessors;
    int    jStatus;
    time_t submitTime;
    time_t beginTime;
    time_t termTime;
    time_t startTime;
    time_t endTime;
    char   queue[MAX_LSB_NAME_LEN];
    char   *resReq;
    char   fromHost[MAXHOSTNAMELEN];
    char   cwd[MAXPATHLEN];
    char   inFile[MAXFILENAMELEN];
    char   outFile[MAXFILENAMELEN];
    char   errFile[MAXFILENAMELEN];
    char   inFileSpool[MAXFILENAMELEN];
    char   commandSpool[MAXFILENAMELEN];
    char   jobFile[MAXFILENAMELEN];
    int    numAskedHosts;
    char   **askedHosts;
    float  hostFactor;
    int    numExHosts;
    char   **execHosts;
    float  cpuTime;
    char   jobName[MAXLINELEN];
    char   command[MAXLINELEN];
    struct  lsfRusage lsfRusage;
    char   *dependCond;
    char   *preExecCmd;
    char   *postExecCmd;
    char   *mailUser;
    char   *projectName;
    int    exitStatus;
    int    maxNumProcessors;
    char   *loginShell;
    int    idx;
    int    maxRMem;
    int    maxRSwap;
};

struct loadIndexLog {
    int nIdx;
    char **name;
};

struct jobMsgLog {
    int usrId;
    int jobId;
    int msgId;
    int type;
    char *src;
    char *dest;
    char *msg;
    int    idx;
};

struct jobMsgAckLog {
    int usrId;
    int jobId;
    int msgId;
    int type;
    char *src;
    char *dest;
    char *msg;
    int    idx;
};

struct jobForceRequestLog {
    int     userId;
    int     numExecHosts;
    char**  execHosts;
    int     jobId;
    int     idx;
    int     options;
    char    userName[MAX_LSB_NAME_LEN];
};

union  eventLog {
    struct jobNewLog jobNewLog;
    struct jobStartLog jobStartLog;
    struct jobStatusLog jobStatusLog;
    struct sbdJobStatusLog sbdJobStatusLog;
    struct jobSwitchLog jobSwitchLog;
    struct jobMoveLog jobMoveLog;
    struct queueCtrlLog queueCtrlLog;
    struct newDebugLog  newDebugLog;
    struct hostCtrlLog hostCtrlLog;
    struct mbdStartLog mbdStartLog;
    struct mbdDieLog mbdDieLog;
    struct unfulfillLog unfulfillLog;
    struct jobFinishLog jobFinishLog;
    struct loadIndexLog loadIndexLog;
    struct migLog migLog;
    struct signalLog signalLog;
    struct jobExecuteLog jobExecuteLog;
    struct jobMsgLog jobMsgLog;
    struct jobMsgAckLog jobMsgAckLog;
    struct jobRequeueLog jobRequeueLog;
    struct chkpntLog chkpntLog;
    struct sigactLog sigactLog;
    struct jobStartAcceptLog jobStartAcceptLog;
    struct jobCleanLog jobCleanLog;
    struct jobForceRequestLog jobForceRequestLog;
    struct logSwitchLog logSwitchLog;
    struct jobModLog jobModLog;
    struct jobAttrSetLog jobAttrSetLog;
};


struct eventRec {
    char   version[MAX_VERSION_LEN];
    int    type;
    time_t eventTime;
    union  eventLog eventLog;
};

struct eventLogFile {
    char eventDir[MAXFILENAMELEN];
    time_t beginTime, endTime;
};

struct eventLogHandle {
    FILE *fp;
    char openEventFile[MAXFILENAMELEN];
    int curOpenFile;
    int lastOpenFile;
};


#define LSF_JOBIDINDEX_FILENAME "lsb.events.index"
#define LSF_JOBIDINDEX_FILETAG "#LSF_JOBID_INDEX_FILE"

struct jobIdIndexS {
    char fileName[MAXFILENAMELEN];
    FILE *fp;
    float version;
    int totalRows;
    time_t lastUpdate;
    int curRow;


    time_t timeStamp;
    LS_LONG_INT minJobId;
    LS_LONG_INT maxJobId;
    int totalJobIds;
    int *jobIds;
};

struct sortIntList {
    int value;
    struct sortIntList *forw;
    struct sortIntList *back;
};

#define LSB_MAX_SD_LENGTH 128
struct lsbMsgHdr {
    int                usrId;
    LS_LONG_INT        jobId;
    int                msgId;
    int                type;
    char               *src;
    char               *dest;
};

struct lsbMsg {
    struct lsbMsgHdr * header;
    char *             msg;
};


#define CONF_NO_CHECK    	0x00
#define CONF_CHECK      	0x01
#define CONF_EXPAND     	0X02
#define CONF_RETURN_HOSTSPEC    0X04
#define CONF_NO_EXPAND    	0X08

struct paramConf {
    struct parameterInfo *param;
};

struct userConf {
    int         numUgroups;
    struct groupInfoEnt *ugroups;
    int         numUsers;
    struct userInfoEnt *users;
};

struct hostConf {
    int       numHosts;
    struct hostInfoEnt *hosts;
    int       numHgroups;
    struct groupInfoEnt *hgroups;
};

typedef struct lsbSharedResourceInstance {
    char *totalValue;
    char *rsvValue;
    int  nHosts;
    char **hostList;

} LSB_SHARED_RESOURCE_INST_T;

typedef struct lsbSharedResourceInfo {
    char *resourceName;
    int  nInstances;
    LSB_SHARED_RESOURCE_INST_T  *instances;
} LSB_SHARED_RESOURCE_INFO_T;

struct queueConf {
    int       numQueues;
    struct queueInfoEnt *queues;
};

struct controlReq {
    int         opCode;  
    char        *name;
    char        *msg;
};

#define  IS_PEND(s)  (((s) & JOB_STAT_PEND) || ((s) & JOB_STAT_PSUSP))

#define  IS_START(s)  (((s) & JOB_STAT_RUN) || ((s) & JOB_STAT_SSUSP) \
		       || ((s) & JOB_STAT_USUSP))

#define  IS_FINISH(s) (((s) & JOB_STAT_DONE) || ((s) & JOB_STAT_EXIT))

#define  IS_SUSP(s) (((s) & JOB_STAT_PSUSP) || ((s) & JOB_STAT_SSUSP) \
                             ||  ((s) & JOB_STAT_USUSP))

#define  IS_POST_DONE(s) ( ( (s) & JOB_STAT_PDONE) == JOB_STAT_PDONE )
#define  IS_POST_ERR(s) ( ( (s) & JOB_STAT_PERR) == JOB_STAT_PERR )
#define  IS_POST_FINISH(s) ( IS_POST_DONE(s) || IS_POST_ERR(s) )

extern int lsberrno;

extern int lsb_mbd_version;

#define PRINT_SHORT_NAMELIST  0x01
#define PRINT_LONG_NAMELIST   0x02
#define PRINT_MCPU_HOSTS      0x04

typedef struct nameList {
     int    listSize;
     char **names;
     int   *counter;
} NAMELIST;

extern NAMELIST * lsb_parseShortStr(char *, int);
extern NAMELIST * lsb_parseLongStr(char *);
extern char * lsb_printNameList(NAMELIST *, int );
extern NAMELIST * lsb_compressStrList(char **, int );
extern char * lsb_splitName(char *, unsigned int *);


#if defined(__STDC__)
#define P_(s) s
#else
#define P_(s) ()
#endif


extern struct paramConf *lsb_readparam P_((struct lsConf *));
extern struct userConf * lsb_readuser  P_((struct lsConf *, int,
					  struct clusterConf *));
extern struct userConf * lsb_readuser_ex P_((struct lsConf *, int,
					     struct clusterConf *,
					     struct sharedConf *));
extern struct hostConf *lsb_readhost P_((struct lsConf *, struct lsInfo *, int,
					 struct clusterConf *));
extern struct queueConf *lsb_readqueue P_((struct lsConf *, struct lsInfo *,
					   int, struct sharedConf *));
extern void updateClusterConf(struct clusterConf *);


extern int lsb_init P_((char *appName));
extern int lsb_openjobinfo P_((LS_LONG_INT, char *, char *, char *, char *,
			       int));
extern struct jobInfoHead *lsb_openjobinfo_a P_((LS_LONG_INT, char *,char *,
						 char *, char *, int));
extern struct jobInfoEnt *lsb_readjobinfo P_((int *));
extern LS_LONG_INT lsb_submit P_((struct submit  *, struct submitReply *));


extern void lsb_closejobinfo P_((void));

extern int  lsb_hostcontrol P_((char *, int, char *));
extern struct queueInfoEnt *lsb_queueinfo P_((char **queues, int *numQueues, char *host, char *userName, int options));
extern int  lsb_reconfig P_((struct controlReq *));
extern int  lsb_signaljob P_((LS_LONG_INT, int));
extern int  lsb_msgjob P_((LS_LONG_INT, char *));
extern int  lsb_chkpntjob P_((LS_LONG_INT, time_t, int));
extern int  lsb_deletejob P_((LS_LONG_INT, int, int));
extern int  lsb_forcekilljob P_((LS_LONG_INT));
extern int  lsb_requeuejob P_((struct jobrequeue *));
extern char *lsb_sysmsg P_((void));
extern void lsb_perror P_((char *));
extern char *lsb_sperror P_((char *));
extern char *lsb_peekjob P_((LS_LONG_INT));

extern int lsb_mig P_((struct submig *, int *badHostIdx));

extern struct hostInfoEnt *lsb_hostinfo P_(( char **, int *));
extern struct hostInfoEnt *lsb_hostinfo_ex P_(( char **, int *, char *, int));
extern int lsb_movejob P_((LS_LONG_INT jobId, int *, int));
extern int lsb_switchjob P_((LS_LONG_INT jobId, char *queue));
extern int lsb_queuecontrol P_((char *, int, char *));
extern struct userInfoEnt *lsb_userinfo P_(( char **, int *));
extern struct groupInfoEnt *lsb_hostgrpinfo P_((char**, int *, int));
extern struct groupInfoEnt *lsb_usergrpinfo P_((char **, int *, int));
extern struct parameterInfo *lsb_parameterinfo P_((char **, int *, int));
extern LS_LONG_INT lsb_modify P_((struct submit *, struct submitReply *, LS_LONG_INT));
extern float * getCpuFactor P_((char *, int));
extern char *lsb_suspreason P_((int, int, struct loadIndexLog *));
extern char *lsb_pendreason P_((int, int *, struct jobInfoHead *,
                            struct loadIndexLog *));

extern int lsb_puteventrec P_((FILE *, struct eventRec *));
extern struct eventRec *lsb_geteventrec P_((FILE *, int *));
extern struct lsbSharedResourceInfo *lsb_sharedresourceinfo P_((char **, int *, char *, int));

extern int lsb_runjob P_((struct runJobRequest*));

extern char *lsb_jobid2str P_((LS_LONG_INT));
extern char *lsb_jobidinstr P_((LS_LONG_INT));
extern void jobId32To64 P_((LS_LONG_INT*, int, int));
extern void jobId64To32 P_((LS_LONG_INT, int*, int*));
extern int lsb_setjobattr(int, struct jobAttrInfoEnt *);

extern LS_LONG_INT lsb_rexecv(int, char **, char **, int *, int);
extern int lsb_catch(const char *, int (*)(void *));
extern void lsb_throw(const char *, void *);

struct sortIntList * initSortIntList(int);
int insertSortIntList(struct sortIntList *, int);
struct sortIntList * getNextSortIntList(struct sortIntList *, struct sortIntList *, int *);
void freeSortIntList(struct sortIntList *);
int getMinSortIntList(struct sortIntList *, int *);
int getMaxSortIntList(struct sortIntList *, int *);
int getTotalSortIntList(struct sortIntList *);

int updateJobIdIndexFile (char *, char *, int);


#undef P_

#endif

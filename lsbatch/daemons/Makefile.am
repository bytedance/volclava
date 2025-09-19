#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) openlava foundation
#
INCLUDES = -I$(top_srcdir)/lsf  -I$(top_srcdir)/lsf/lib \
           -I$(top_srcdir)/lsbatch  -I$(top_srcdir)/lsbatch/lib -I./

sbin_PROGRAMS = mbatchd sbatchd
mbatchd_SOURCES  = \
mbd.comm.c mbd.host.c mbd.jgrp.c mbd.main.c mbd.proxy.c mbd.resource.c \
mbd.dep.c mbd.init.c mbd.job.c mbd.misc.c mbd.queue.c mbd.serv.c \
mbd.grp.c mbd.jarray.c mbd.log.c mbd.requeue.c mbd.window.c mbd.fairshare.c\
elock.c misc.c mail.c daemons.c daemons.xdr.c qmbd.c\
mbd.h daemonout.h daemons.h jgrp.h proxy.h mbd.profcnt.def mbd.fairshare.h 
if SCHED_EXPERIMENTAL
mbatchd_SOURCES += mbd.epolicy.c
else
mbatchd_SOURCES += mbd.policy.c
endif

mbatchd_LDFLAGS = -pthread
mbatchd_LDADD = ../lib/liblsbatch.a \
                ../../lsf/lib/liblsf.a \
                ../../lsf/intlib/liblsfint.a -lm
if !CYGWIN
mbatchd_LDADD += -lnsl
endif

sbatchd_SOURCES = sbd.comm.c sbd.file.c sbd.job.c sbd.main.c \
                  sbd.misc.c sbd.policy.c sbd.serv.c sbd.sig.c sbd.xdr.c \
                  elock.c mail.c misc.c daemons.c daemons.xdr.c \
                  sbd.h daemonout.h daemons.h 

sbatchd_LDADD = ../lib/liblsbatch.a \
                ../../lsf/lib/liblsf.a \
                ../../lsf/intlib/liblsfint.a -lm
if !CYGWIN
sbatchd_LDADD += -lnsl
endif

etags :
	etags *.[hc] ../*.h ../lib/*.[hc] ../../lsf/*.h \
  	../../lsf/lib/*.[hc] ../../lsf/intlib/*.[hc]

#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf  -I$(top_srcdir)/lsf/lib \
           -I$(top_srcdir)/lsbatch  -I$(top_srcdir)/lsbatch/lib -I./

sbin_PROGRAMS = mbatchd sbatchd
mbatchd_SOURCES  = \
mbd.policy.c mbd.comm.c mbd.host.c mbd.jgrp.c mbd.main.c mbd.proxy.c \
mbd.resource.c mbd.dep.c mbd.init.c mbd.job.c mbd.misc.c mbd.queue.c \
mbd.serv.c mbd.grp.c mbd.jarray.c mbd.log.c mbd.requeue.c mbd.window.c \
mbd.fairshare.c elock.c misc.c mail.c daemons.c daemons.xdr.c \
mbd.h daemonout.h daemons.h jgrp.h proxy.h mbd.profcnt.def mbd.fairshare.h

mbatchd_LDADD = ../lib/liblsbatch.a \
                ../../lsf/lib/liblsf.a \
                ../../lsf/intlib/liblsfint.a -lm
sbatchd_SOURCES = sbd.comm.c sbd.file.c sbd.job.c sbd.main.c \
                  sbd.misc.c sbd.policy.c sbd.serv.c sbd.sig.c sbd.xdr.c \
                  elock.c mail.c misc.c daemons.c daemons.xdr.c \
                  sbd.h daemonout.h daemons.h

sbatchd_LDADD = ../lib/liblsbatch.a \
                ../../lsf/lib/liblsf.a \
                ../../lsf/intlib/liblsfint.a -lm

etags :
	etags *.[hc] ../*.h ../lib/*.[hc] ../../lsf/*.h \
  	../../lsf/lib/*.[hc] ../../lsf/intlib/*.[hc]

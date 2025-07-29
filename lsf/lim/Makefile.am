#
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf -D$(HOSTTYPE) -DHOST_TYPE_STRING=\"$(HOSTTYPE)\"

HOSTTYPE=LINUX64

sbin_PROGRAMS = lim
lim_SOURCES  = \
lim.cluster.c lim.control.c lim.internal.c lim.main.c lim.policy.c \
lim.xdr.c lim.conf.c lim.info.c lim.load.c lim.misc.c  lim.rload.c  \
lim.common.h  lim.conf.h  lim.h  lim.linux.h  limout.h
lim_LDADD =  ../lib/liblsf.a ../intlib/liblsfint.a

etags:
	etags ../*.h *.[hc] ../intlib/*.[hc] ../lib/*.[hc] \
	../res/*.[hc] ../lstools/*.[hc]


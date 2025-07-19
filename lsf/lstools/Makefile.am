#
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I..

bin_PROGRAMS = lsacct lseligible lshosts lsid lsinfo lsloadadj \
 		lsload lsmon lsplace lsrcp lsrun lsaddhost lsrmhost

lsacct_SOURCES = lsacct.c
lsacct_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lseligible_SOURCES = lseligible.c
lseligible_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lshosts_SOURCES = lshosts.c
lshosts_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsid_SOURCES = lsid.c
lsid_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsinfo_SOURCES = lsinfo.c
lsinfo_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsloadadj_SOURCES = lsloadadj.c
lsloadadj_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsload_SOURCES = lsload.c load.c
lsload_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsmon_SOURCES = lsmon.c
lsmon_LDADD = load.o ../lib/liblsf.a ../intlib/liblsfint.a -lncurses

lsplace_SOURCES = lsplace.c
lsplace_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsrcp_SOURCES = lsrcp.c
lsrcp_LDADD = ../lib/liblsf.a ../intlib/liblsfint.a

lsrun_SOURCES = lsrun.c
lsrun_LDADD = ../lib/liblsf.a

lsaddhost_SOURCES = lsaddhost.c
lsaddhost_LDADD = ../lib/liblsf.a

lsrmhost_SOURCES = lsrmhost.c
lsrmhost_LDADD = ../lib/liblsf.a

etags :
	etags *.[hc] ../*.h ../lib/*.[hc]

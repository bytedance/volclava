#
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf  -I$(top_srcdir)/lsf/lib \
           -I$(top_srcdir)/lsbatch  -I$(top_srcdir)/lsbatch/lib -I./

if BUILD_BHIST_SPEEDUP
bin_PROGRAMS = bhist_original
bhist_original_SOURCES = bhist.c read.event.c bhist.h
bhist_original_LDFLAGS = -Wl,--as-needed
bhist_original_LDADD = ../cmd/cmd.job.o \
	../cmd/cmd.misc.o ../cmd/cmd.jobid.o ../cmd/cmd.prt.o \
	../cmd/cmd.err.o ../cmd/cmd.hist.o \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a -lm

bin_SCRIPTS = bhist
.PHONY: force-bhist-wrapper
force-bhist-wrapper:
bhist: $(srcdir)/bhist.in force-bhist-wrapper
	$(AM_V_GEN)cp $(srcdir)/bhist.in $@ && chmod +x $@
CLEANFILES = bhist
else
bin_PROGRAMS = bhist
bhist_SOURCES = bhist.c read.event.c bhist.h
bhist_LDFLAGS = -Wl,--as-needed
bhist_LDADD = ../cmd/cmd.job.o \
	../cmd/cmd.misc.o ../cmd/cmd.jobid.o ../cmd/cmd.prt.o \
	../cmd/cmd.err.o ../cmd/cmd.hist.o \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a -lm
endif

EXTRA_DIST = bhist.in

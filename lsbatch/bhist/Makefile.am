#
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf  -I$(top_srcdir)/lsf/lib \
           -I$(top_srcdir)/lsbatch  -I$(top_srcdir)/lsbatch/lib -I./

bin_PROGRAMS = bhist
bhist_SOURCES  = bhist.c read.event.c bhist.h
bhist_LDADD = ../cmd/cmd.job.o \
	../cmd/cmd.misc.o ../cmd/cmd.jobid.o ../cmd/cmd.prt.o \
	../cmd/cmd.err.o ../cmd/cmd.hist.o \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a -lm

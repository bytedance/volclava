#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf  -I$(top_srcdir)/lsf/lib \
           -I$(top_srcdir)/lsbatch  -I$(top_srcdir)/lsbatch/lib -I./

bin_PROGRAMS = badmin bkill bparams brestart btop bbot bmgroup \
bpeek brun busers bhosts bmig bqueues bsub bjobs bmod \
brequeue bswitch

badmin_SOURCES = badmin.c cmd.bqc.c cmd.hist.c \
	cmd.bhc.c cmd.misc.c cmd.job.c cmd.prt.c \
	badmin.h cmd.h
badmin_LDADD = ../../lsf/lsadm/startup.o \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a -lm

bkill_SOURCES = bkill.c cmd.sig.c cmd.jobid.c cmd.err.c
bkill_LDADD = \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bparams_SOURCES = bparams.c cmd.h
bparams_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

brestart_SOURCES = brestart.c cmd.sub.c cmd.jobid.c \
	cmd.err.c cmd.h
brestart_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

btop_SOURCES = btop.c cmd.move.c cmd.jobid.c cmd.misc.c \
	 cmd.prt.c cmd.err.c cmd.h
btop_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bbot_SOURCES = bbot.c cmd.move.c cmd.jobid.c cmd.misc.c \
	 cmd.prt.c cmd.err.c cmd.h
bbot_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bmgroup_SOURCES = bmgroup.c cmd.misc.c cmd.h
bmgroup_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bpeek_SOURCES = bpeek.c cmd.err.c cmd.jobid.c cmd.misc.c cmd.prt.c cmd.h
bpeek_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

brun_SOURCES = brun.c cmd.jobid.c cmd.err.c cmd.h
brun_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

busers_SOURCES = busers.c cmd.misc.c
busers_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bhosts_SOURCES = bhosts.c cmd.prt.c cmd.misc.c cmd.h
bhosts_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bmig_SOURCES = bmig.c cmd.jobid.c cmd.err.c cmd.h
bmig_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bqueues_SOURCES = bqueues.c cmd.prt.c cmd.misc.c cmd.h
bqueues_LDADD = \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bsub_SOURCES = bsub.c cmd.sub.c cmd.jobid.c cmd.err.c cmd.h
bsub_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bjobs_SOURCES = bjobs.c cmd.prt.c cmd.err.c cmd.job.c \
	cmd.jobid.c cmd.misc.c cmd.h cJSON.c cJSON.h
bjobs_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bmod_SOURCES = bmod.c cmd.sub.c cmd.jobid.c cmd.err.c cmd.h
bmod_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

brequeue_SOURCES = brequeue.c cmd.jobid.c cmd.err.c cmd.h
brequeue_LDADD =   \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

bswitch_SOURCES = bswitch.c cmd.jobid.c cmd.err.c cmd.h
bswitch_LDADD =  \
	../lib/liblsbatch.a \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a  -lm

install-data-local:
	cd "$(DESTDIR)$(bindir)" && ln -sf bkill bstop
	cd "$(DESTDIR)$(bindir)" && ln -sf bkill bresume
	cd "$(DESTDIR)$(bindir)" && ln -sf bkill bchkpnt
	cd "$(DESTDIR)$(bindir)" && ln -sf bmgroup bugroup

etags :
	etags *.[hc] ../*.h ../lib/*.[hc] ../../lsf/*.h ../../lib/*.[hc] \
	../../intlib/*.[hc]


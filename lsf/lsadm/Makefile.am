#
# Copyright (C) openlava foundation
#
AM_CPPFLAGS = -I$(top_srcdir)/lsf

bin_PROGRAMS = lsadmin

lsadmin_SOURCES = lsadmin.c startup.c cmd.limcontrol.c \
                   cmd.rescontrol.c lsadmin.h
lsadmin_LDADD =  ../lib/liblsf.a ../intlib/liblsfint.a


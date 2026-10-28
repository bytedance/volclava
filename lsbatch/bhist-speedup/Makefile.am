#
# Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
#
# bhist speedup is built as part of Volclava and uses the selected cluster
# environment from LSF_ENVDIR.
#
# All module files carry the bhist. prefix so includes such as
# "bhist.network.h" identify their module at the reference site.
#

AM_CPPFLAGS = \
	-D_POSIX_C_SOURCE=200809L \
	-I$(top_srcdir)/lsf \
	-I$(top_srcdir)/lsf/lib \
	-I$(top_srcdir)/lsf/intlib \
	-I$(top_srcdir)/lsbatch \
	-I$(top_srcdir)/lsbatch/lib \
	-I$(top_srcdir)/lsbatch/bhist-speedup/include

VOLCLAVA_LIBS = \
	../../lsf/lib/liblsf.a \
	../../lsf/intlib/liblsfint.a

CONFIG_SOURCES = src/bhist.config.c
SERVICE_SOURCES = src/bhist.service.c
UTIL_SOURCES = src/bhist.util.c

# bhist-fast is the user-facing client; loader and server are services
# managed by the administrator, so they install under sbin like mbatchd.
bin_PROGRAMS = \
	bhist-fast

sbin_PROGRAMS = \
	bhist-speedup-server \
	bhist-speedup-loader

# bhist-admin starts, stops and queries the loader and server;
# bhist-speedup-setup generates their systemd units.
dist_bin_SCRIPTS = \
	scripts/bhist-admin \
	scripts/bhist-speedup-setup

bhist_fast_SOURCES = \
	src/bhist.client.main.c \
	src/bhist.network.c \
	src/bhist.protocol.c \
	$(CONFIG_SOURCES) \
	$(UTIL_SOURCES)
bhist_fast_LDFLAGS = -Wl,--as-needed
bhist_fast_LDADD = $(VOLCLAVA_LIBS) -lm

bhist_speedup_server_SOURCES = \
	src/bhist.server.main.c \
	src/bhist.server.query.c \
	src/bhist.server.control.c \
	src/bhist.network.c \
	src/bhist.protocol.c \
	$(CONFIG_SOURCES) \
	$(SERVICE_SOURCES) \
	$(UTIL_SOURCES)
bhist_speedup_server_LDFLAGS = -Wl,--as-needed
bhist_speedup_server_LDADD = $(VOLCLAVA_LIBS) $(SQLITE3_LIBS) -lm

bhist_speedup_loader_SOURCES = \
	$(CONFIG_SOURCES) \
	$(SERVICE_SOURCES) \
	$(UTIL_SOURCES) \
	src/bhist.loader.main.c \
	src/bhist.loader.events.c \
	src/bhist.loader.db.c
bhist_speedup_loader_CFLAGS = -pthread
bhist_speedup_loader_LDFLAGS = -pthread -Wl,--as-needed
bhist_speedup_loader_LDADD = $(VOLCLAVA_LIBS) $(SQLITE3_LIBS) -lm

# The bhist-speedup(1) manual lives with the other user commands in
# ../man1 and is installed from there.

noinst_HEADERS = \
	include/bhist.config.h \
	include/bhist.loader.h \
	include/bhist.network.h \
	include/bhist.protocol.h \
	include/bhist.service.h \
	include/bhist.server.h \
	include/bhist.util.h

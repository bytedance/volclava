#
# bhist speed up is built as part of Volclava and uses the selected cluster
# environment from LSF_ENVDIR.
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

CONFIG_SOURCES = src/config.c
DAEMON_SOURCES = src/daemon.c

bin_PROGRAMS = \
	bhist-speedup \
	bhist-speedup-server \
	bhist-speedup-loader

bhist_speedup_SOURCES = \
	src/client.main.c \
	src/network.c \
	src/protocol.c \
	$(CONFIG_SOURCES)
bhist_speedup_LDFLAGS = -Wl,--as-needed
bhist_speedup_LDADD = $(VOLCLAVA_LIBS) -lm

bhist_speedup_server_SOURCES = \
	src/server.main.c \
	src/server.query.c \
	src/server.control.c \
	src/network.c \
	src/protocol.c \
	$(CONFIG_SOURCES) \
	$(DAEMON_SOURCES)
bhist_speedup_server_LDFLAGS = -Wl,--as-needed
bhist_speedup_server_LDADD = $(VOLCLAVA_LIBS) $(SQLITE3_LIBS) -lm

bhist_speedup_loader_SOURCES = \
	$(CONFIG_SOURCES) \
	$(DAEMON_SOURCES) \
	src/loader.main.c \
	src/loader.events.c \
	src/loader.db.c
bhist_speedup_loader_CFLAGS = -pthread
bhist_speedup_loader_LDFLAGS = -pthread -Wl,--as-needed
bhist_speedup_loader_LDADD = $(VOLCLAVA_LIBS) $(SQLITE3_LIBS) -lm

man_MANS = man/bhist-speedup.1

noinst_HEADERS = \
	include/bhist.config.h \
	include/loader.h \
	include/network.h \
	include/protocol.h \
	include/daemon.h \
	include/server.h \
	include/version.h

EXTRA_DIST = man/bhist-speedup.1

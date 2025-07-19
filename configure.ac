# configure.ac for volclava — compatible and minimal
AC_INIT([volclava], [2.0], [support@example.com])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([lsf/Makefile.am])
AC_PREFIX_DEFAULT([/opt/volclava-2.0])

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_RANLIB
AC_PROG_YACC
AC_PROG_LEX
AC_PROG_LN_S
AC_PROG_MAKE_SET

# GCC warnings
if test "$GCC" = "yes"; then
  CFLAGS="$CFLAGS -Wall -fPIC -Wno-error=format-security"
fi

# Optional build-time overrides
AC_ARG_VAR([volclavaadmin], [volclava admin binary name])
AC_ARG_VAR([volclavacluster], [volclava cluster name])
AC_SUBST([volclavaadmin])
AC_SUBST([volclavacluster])

# Add libtirpc headers
CPPFLAGS="$CPPFLAGS -I/usr/include/tirpc"

# Check for XDR headers and libtirpc
AC_CHECK_HEADERS([rpc/xdr.h], [], [AC_MSG_ERROR([cannot build volclava: missing rpc/xdr.h])])
AC_SEARCH_LIBS([xdrmem_create], [tirpc], [], [AC_MSG_ERROR([missing -ltirpc])])

# Check for libnsl (NIS)
AC_CHECK_LIB([nsl], [yp_get_default_domain], [LIBS="$LIBS -lnsl"], [AC_MSG_ERROR([missing libnsl])])

# Check Tcl headers
AC_CHECK_HEADERS([tcl.h], [], [
  AC_CHECK_HEADERS([tcl/tcl.h], [], [AC_MSG_ERROR([missing tcl.h])])
])

# Tcl library search
AC_CHECK_LIB([tcl], [Tcl_CreateInterp], [], [
  for v in 8.6 8.5 8.4 8.3 8.2 8.1 8.0; do
    AC_CHECK_LIB([tcl$v], [Tcl_CreateInterp], [LIBS="$LIBS -ltcl$v"; break])
  done
])

# Output Makefiles
AC_CONFIG_FILES([
  Makefile
  config/Makefile
  lsf/Makefile
  lsf/intlib/Makefile
  lsf/lib/Makefile
  lsf/lim/Makefile
  lsf/res/Makefile
  lsf/pim/Makefile
  lsf/lstools/Makefile
  lsf/lsadm/Makefile
  lsf/man/Makefile
  lsf/man/man1/Makefile
  lsf/man/man5/Makefile
  lsf/man/man8/Makefile
  lsbatch/Makefile
  lsbatch/lib/Makefile
  lsbatch/cmd/Makefile
  lsbatch/bhist/Makefile
  lsbatch/daemons/Makefile
  lsbatch/man1/Makefile
  lsbatch/man5/Makefile
  lsbatch/man8/Makefile
  eauth/Makefile
  scripts/Makefile
  chkpnt/Makefile
  config/lsf.conf
  config/lsb.hosts
  config/lsb.params
  config/lsb.queues
  config/lsb.users
  config/lsf.cluster.volclava
  config/lsf.shared
  config/lsf.task
  config/volclava.csh
  config/volclava
  config/volclava.setup
  config/volclava.sh
])

AC_OUTPUT

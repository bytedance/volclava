#
# volclava project
#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) 2011 David Bigagli
#
AC_INIT(volclava, 2.1)
AC_CONFIG_HEADERS(config.h)
AC_PREFIX_DEFAULT([/opt/volclava-2.1])


# Set some cluster configuration variables .
AC_ARG_VAR([volclavaadmin], "customize cluster administrator")
if test -z "$volclavaadmin"; then
    volclavaadmin="volclava"
fi
AC_SUBST([volclavaadmin])

AC_ARG_VAR([volclavacluster], "customize cluster name")
if test -z "$volclavacluster"; then
    volclavacluster="volclava"
fi
AC_SUBST([volclavacluster])

# Cygwin support
AC_CANONICAL_SYSTEM
case "${target_os}" in
  *cygwin*)
    AM_CONDITIONAL([CYGWIN], [true]) ;;
  *)
    AM_CONDITIONAL([CYGWIN], [false]) ;;
esac

# Initialize the automake
AM_INIT_AUTOMAKE

# Expirimental scheduling module
AM_CONDITIONAL([SCHED_EXPERIMENTAL], [false])

# Clean output if possible
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

# Check for programs
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_RANLIB
AC_PROG_YACC
AC_PROG_LEX
AC_PROG_LN_S
AC_PROG_MAKE_SET

#AC_DEFINE(REL_DATE, "Jan 31 2012", "Set the release date.")

# Force warnings on for gcc
if test "x$ac_compiler_gnu" = xyes; then
    CFLAGS="$CFLAGS -Wall -fPIC -Wno-error=format-security"
fi

CFLAGS="$CFLAGS -I/usr/include/tirpc"

AC_CHECK_HEADERS([rpc/xdr.h], [], [AC_CHECK_HEADERS([tirpc/rpc/xdr.h], [CFLAGS="-I/usr/include/tirpc"], AC_MSG_ERROR([cannot build volclava without xdr])]))
AC_SEARCH_LIBS([xdr_int], [rpc tirpc], [], [AC_MSG_ERROR([cannot build volclava without XDR])])

#AC_CHECK_HEADERS(ncurses.h,[cf_cv_ncurses_header="ncurses.h"])
# Check for tcl, we try tcl and all 8.X versions
AC_CHECK_HEADERS([tcl.h], [], [AC_CHECK_HEADERS([tcl/tcl.h], [], AC_MSG_ERROR([cannot build volclava without tcl])]))
AC_CHECK_LIB([tcl], [Tcl_CreateInterp], [], [
 AC_CHECK_LIB([tcl8.6], [Tcl_CreateInterp], [], [
    AC_CHECK_LIB([tcl8.5], [Tcl_CreateInterp], [], [
        AC_CHECK_LIB([tcl8.4], [Tcl_CreateInterp], [], [
            AC_CHECK_LIB([tcl8.3], [Tcl_CreateInterp], [], [
                AC_CHECK_LIB([tcl8.2], [Tcl_CreateInterp], [], [
                    AC_CHECK_LIB([tcl8.1], [Tcl_CreateInterp], [], [
                        AC_CHECK_LIB([tcl8.0], [Tcl_CreateInterp], [], [
                            AC_MSG_ERROR([cannot build volclava without tcl8.*])
])])])])])])])])

CLUSTERFILE=config/lsf.cluster.${volclavacluster}:config/lsf.cluster.in
AC_CONFIG_FILES([
	Makefile                 \
	config/Makefile          \
	lsf/Makefile             \
	lsf/intlib/Makefile      \
	lsf/lib/Makefile         \
	lsf/lim/Makefile         \
	lsf/res/Makefile         \
	lsf/pim/Makefile         \
	lsf/lstools/Makefile     \
	lsf/lsadm/Makefile       \
	lsf/man/Makefile         \
	lsf/man/man1/Makefile    \
	lsf/man/man5/Makefile    \
	lsf/man/man8/Makefile    \
	lsbatch/Makefile         \
	lsbatch/lib/Makefile     \
	lsbatch/cmd/Makefile     \
	lsbatch/bhist/Makefile   \
	lsbatch/daemons/Makefile \
	lsbatch/man1/Makefile    \
	lsbatch/man5/Makefile    \
	lsbatch/man8/Makefile    \
	eauth/Makefile           \
	scripts/Makefile         \
	chkpnt/Makefile          \
	config/lsf.conf          \
	config/lsb.hosts         \
	config/lsb.params        \
	config/lsb.queues        \
	config/lsb.users         \
        ${CLUSTERFILE} \
	config/lsf.shared        \
	config/lsf.task          \
	config/volclava.csh      \
	config/volclava          \
	config/volclava.setup    \
	config/volclava.sh]
)

AC_OUTPUT

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

# Set platform type
OS_NAME="unknown"
OS_VERSION="unknown"
CPU_ARCH="unknown"
if test -f /etc/os-release; then
    . /etc/os-release
    
    #check ubuntu
    if test "$ID" = "ubuntu" || test "$ID_LIKE" = "ubuntu"; then
        OS_NAME="ubuntu"
        OS_VERSION=$VERSION_ID
    
    #check centos
    elif test "$ID" = "centos"; then
        OS_NAME="centos"
        OS_VERSION=$VERSION_ID
    
    #check rocky
    elif test "$ID" = "rocky"; then
        OS_NAME="rocky"
        OS_VERSION=$VERSION_ID
    
    #check redhat
    elif test "$ID" = "rhel" || test "$ID_LIKE" = "rhel" || test "$NAME" = "Red Hat Enterprise Linux"; then
        OS_NAME="redhat"
        OS_VERSION=$VERSION_ID
    fi
else #Compatible with older systems without /etc/os-release
    if test -f /etc/redhat-release; then
        RELEASE=$(cat /etc/redhat-release)
        # Check CentOS
        if echo "$RELEASE" | grep -qi "centos"; then
            OS_NAME="centos"
            OS_VERSION=$(echo "$RELEASE" | awk '{print $4}' | cut -d '.' -f 1)
        # Check RedHat
        elif echo "$RELEASE" | grep -qi "red hat"; then
            OS_NAME="redhat"
            OS_VERSION=$(echo "$RELEASE" | awk '{print $7}' | cut -d '.' -f 1)
        fi
    elif test -f /etc/lsb-release; then
        . /etc/lsb-release
        if test "$DISTRIB_ID" = "Ubuntu"; then
            OS_NAME="ubuntu"
            OS_VERSION=$DISTRIB_RELEASE
        fi
    fi
fi
OS_VERSION=$(echo "$OS_VERSION" | sed -E 's/[^0-9.]//g' | cut -d '.' -f 1)
CPU_ARCH=$(uname -m)
PLATFORM="${OS_NAME}-${OS_VERSION}-${CPU_ARCH}"
AC_SUBST([PLATFORM])

# Initialize bin/sbin/lib with platform information
bindir="${prefix}/exec/${PLATFORM}/bin"
sbindir="${prefix}/exec/${PLATFORM}/sbin"
libdir="${prefix}/exec/${PLATFORM}/lib"
AC_SUBST(bindir)
AC_SUBST(sbindir)
AC_SUBST(libdir)

# Control installation in interactive mode or not, default is interactive mode
AC_ARG_ENABLE([interactive-install],
              [AS_HELP_STRING([--disable-interactive-install],
              [disable interactive configuration file installation])],
              [enable_interactive_install=$enableval],
              [enable_interactive_install=yes])
if test "x$enable_interactive_install" = "xyes"; then
    INTERACTIVE=1
else
    INTERACTIVE=0
fi
AC_SUBST([INTERACTIVE])

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

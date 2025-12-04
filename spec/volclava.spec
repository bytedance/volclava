#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) 2011-2012 David Bigagli
# Copyright (C) 2007 Platform Computing Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of version 2 of the GNU General Public License as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
#
#

%define major 2
%define minor 1
%define release 1
%define build_timestamp %(date +"%Y%m%d")

%define version %{major}.%{minor}
%define _volclavatop /opt/volclava-%{version}
%define _libdir %{_volclavatop}/lib
%define _bindir %{_volclavatop}/bin
%define _sbindir %{_volclavatop}/sbin
%define _mandir %{_volclavatop}/share/man
%define _logdir %{_volclavatop}/log
%define _includedir %{_volclavatop}/include
%define _etcdir %{_volclavatop}/etc

%define VOLCADMIN %( \
        if [ -n "$volclavaadmin" ]; then \
            echo "$volclavaadmin"; \
        else \
            echo "volclava"; \
        fi \
)
%define CLUSTERNAME %( \
        if [ -n "$volclavacluster" ]; then \
            echo "$volclavacluster"; \
        else \
            echo "volclava"; \
        fi \
)

Summary: volclava Distributed Batch Scheduler
Name: volclava
Version: %{version}
# Release: 0.b.%{build_timestamp}
Release: %{release}.20251116
License: GPLv2
Group: Applications/Productivity
Vendor: volclava foundation
ExclusiveArch: x86_64
URL: https://www.bytedance.com/
Source: %{name}-%{version}.tar.gz
Buildroot: %{_tmppath}/%{name}-%{version}-buildroot
BuildRequires: gcc, tcl-devel, ncurses-devel
Requires: ncurses, tcl
Requires(pre): /usr/sbin/useradd
Requires(post): /sbin/chkconfig
Requires(preun): /sbin/chkconfig
Prefix: /opt

%description
volclava Distributed Batch Scheduler

#
# PREP
#
%prep
rm -rf ${RPM_BUILD_ROOT}
mkdir -p ${RPM_BUILD_ROOT}
%setup -q -n %{name}-%{version}

#
# BUILD
#
%build
./bootstrap.sh
make

#
# CLEAN
#
%clean
/bin/rm -rf ${RPM_BUILD_ROOT}

#
# INSTALL
#
%install

# Install binaries, daemons
#make install prefix=${RPM_BUILD_ROOT}%{_volclavatop}

# install directories and files
install -d ${RPM_BUILD_ROOT}%{_bindir}
install -d ${RPM_BUILD_ROOT}%{_etcdir}
install -d ${RPM_BUILD_ROOT}%{_includedir}
install -d ${RPM_BUILD_ROOT}%{_libdir}
install -d ${RPM_BUILD_ROOT}%{_logdir}
install -d ${RPM_BUILD_ROOT}%{_sbindir}
install -d ${RPM_BUILD_ROOT}%{_mandir}/man1
install -d ${RPM_BUILD_ROOT}%{_mandir}/man3
install -d ${RPM_BUILD_ROOT}%{_mandir}/man5
install -d ${RPM_BUILD_ROOT}%{_mandir}/man8
install -d ${RPM_BUILD_ROOT}%{_volclavatop}/work/logdir

# in volclava root
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/COPYING  ${RPM_BUILD_ROOT}%{_volclavatop}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/README.md  ${RPM_BUILD_ROOT}%{_volclavatop}

# bin
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/badmin  ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bbot    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/bhist/bhist   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bhosts  ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bjobs   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bkill   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bmgroup ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bmig    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bmod    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bparams ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bpeek   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bqueues ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/brequeue ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/brestart ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/brun     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bsub     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/bswitch  ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/btop     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/cmd/busers   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/scripts/lam-mpirun ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsacct     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lsadm/lsadmin    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lseligible ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lshosts    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsid       ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsinfo     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsload     ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsloadadj  ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsmon      ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsplace    ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lstools/lsrcp      ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/scripts/mpich2-mpiexec ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/scripts/mpich-mpirun   ${RPM_BUILD_ROOT}%{_bindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/scripts/openmpi-mpirun ${RPM_BUILD_ROOT}%{_bindir}

# etc
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsf.cluster.%{CLUSTERNAME} ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsf.conf ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsf.task ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsf.shared ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsb.params ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsb.queues ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsb.hosts ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/lsb.users ${RPM_BUILD_ROOT}%{_etcdir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/config/volclava.setup ${RPM_BUILD_ROOT}%{_etcdir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/config/volclava ${RPM_BUILD_ROOT}%{_etcdir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/config/volclava.sh ${RPM_BUILD_ROOT}%{_etcdir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/config/volclava.csh ${RPM_BUILD_ROOT}%{_etcdir}

# include
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lsf.h ${RPM_BUILD_ROOT}%{_includedir}
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/lsbatch.h ${RPM_BUILD_ROOT}%{_includedir}

# lib
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lib/liblsf.a  ${RPM_BUILD_ROOT}%{_libdir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/lib/liblsbatch.a  ${RPM_BUILD_ROOT}%{_libdir}

# sbin
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/eauth/eauth  ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/lim/lim  ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/daemons/mbatchd  ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/res/nios  ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/pim/pim  ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/res/res ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/daemons/sbatchd ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/chkpnt/echkpnt          ${RPM_BUILD_ROOT}%{_sbindir}
install -m 755 ${RPM_BUILD_DIR}/%{name}-%{version}/chkpnt/erestart         ${RPM_BUILD_ROOT}%{_sbindir}

# share
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bbot.1    ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bchkpnt.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bhosts.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bjobs.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bkill.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bmgroup.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bmig.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bmod.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bparams.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bpeek.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bqueues.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/brequeue.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/brestart.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bresume.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bstop.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bsub.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/btop.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bugroup.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/busers.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/bswitch.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsacct.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lseligible.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsfbase.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man1/lsfbatch.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsfintro.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lshosts.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsid.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsinfo.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsload.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsloadadj.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsmon.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsplace.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lsrcp.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man1/lstools.1 ${RPM_BUILD_ROOT}%{_mandir}/man1
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.acct.5  ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.events.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.hosts.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.params.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.queues.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man5/lsb.users.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/lim.acct.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/lsf.acct.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/lsf.cluster.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/lsf.conf.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/lsf.shared.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man5/res.acct.5 ${RPM_BUILD_ROOT}%{_mandir}/man5
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man8/badmin.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man8/brun.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/eauth.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/eexec.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/esub.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/lim.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/lsadmin.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/lsfinstall.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man8/mbatchd.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/nios.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/pim.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsf/man/man8/res.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
install -m 644 ${RPM_BUILD_DIR}/%{name}-%{version}/lsbatch/man8/sbatchd.8  ${RPM_BUILD_ROOT}%{_mandir}/man8
#
# PRE
#
%pre

#
# Add admin user
#
/usr/sbin/groupadd -f %{VOLCADMIN} > /dev/null 2>&1 || true
/usr/sbin/useradd -c "volclava Administrator" -g %{VOLCADMIN} -m -d /home/%{VOLCADMIN} %{VOLCADMIN} > /dev/null 2>&1 || true

#
# POST
#
%post

#
# set variables
#
_volclavatop=${RPM_INSTALL_PREFIX}/volclava-%{version}

# create the symbolic links
ln -sf ${_volclavatop}/bin/bkill  ${_volclavatop}/bin/bstop
ln -sf ${_volclavatop}/bin/bkill  ${_volclavatop}/bin/bresume
ln -sf ${_volclavatop}/bin/bkill  ${_volclavatop}/bin/bchkpnt
ln -sf ${_volclavatop}/bin/bmgroup  ${_volclavatop}/bin/bugroup
chown -h %{VOLCADMIN}:%{VOLCADMIN} ${_volclavatop}/bin/bstop
chown -h %{VOLCADMIN}:%{VOLCADMIN} ${_volclavatop}/bin/bresume
chown -h %{VOLCADMIN}:%{VOLCADMIN} ${_volclavatop}/bin/bchkpnt
chown -h %{VOLCADMIN}:%{VOLCADMIN} ${_volclavatop}/bin/bugroup
sed -i "s:/opt/volclava-%{version}:${_volclavatop}:g" ${_volclavatop}/etc/volclava.sh
sed -i "s:/opt/volclava-%{version}:${_volclavatop}:g" ${_volclavatop}/etc/volclava.csh
sed -i "s:/opt/volclava-%{version}:${_volclavatop}:g" ${_volclavatop}/etc/volclava
ln -sf ${_volclavatop}/etc/volclava.sh %{_sysconfdir}/profile.d/volclava.sh
ln -sf ${_volclavatop}/etc/volclava.csh %{_sysconfdir}/profile.d/volclava.csh
/usr/bin/cp --backup=numbered  ${_volclavatop}/etc/volclava %{_sysconfdir}/init.d
sed -i "s:/opt/volclava-%{version}:${_volclavatop}:g" ${_volclavatop}/etc/lsf.conf


# Register lava daemons
/sbin/chkconfig --add volclava
/sbin/chkconfig volclava on

%preun
/sbin/service volclava stop > /dev/null 2>&1
/sbin/chkconfig volclava off
/sbin/chkconfig --del volclava


%postun
_volclavatop=${RPM_INSTALL_PREFIX}/volclava-%{version}
rm -f /etc/init.d/volclava
rm -f /etc/profile.d/volclava.*
rm -f ${_volclavatop}/bin/bstop ${_volclavatop}/bin/bresume ${_volclavatop}/bin/bchkpnt ${_volclavatop}/bin/bugroup
if [ -d "${_volclavatop}/bin" ]; then
    if [ -z "$(ls -A "${_volclavatop}/bin")" ]; then
        rm -rf "${_volclavatop}/bin"
    fi
fi

#
# FILES
#
%files
%defattr(-,%{VOLCADMIN},%{VOLCADMIN})
%attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_etcdir}/volclava
%{_etcdir}/volclava.sh
%{_etcdir}/volclava.csh
%{_etcdir}/volclava.setup
%{_sbindir}/eauth
%{_sbindir}/echkpnt
%{_sbindir}/erestart
%{_sbindir}/mbatchd
%{_sbindir}/sbatchd
%{_sbindir}/lim
%{_sbindir}/res
%{_sbindir}/pim
%{_sbindir}/nios
%{_bindir}/badmin
%{_bindir}/lsadmin
%{_bindir}/bbot
%{_bindir}/bhist
%{_bindir}/bhosts
%{_bindir}/bjobs
%{_bindir}/bkill
%{_bindir}/bmgroup
%{_bindir}/bmig
%{_bindir}/bmod
%{_bindir}/bparams
%{_bindir}/bpeek
%{_bindir}/bqueues
%{_bindir}/brequeue
%{_bindir}/brestart
%{_bindir}/brun
%{_bindir}/bsub
%{_bindir}/bswitch
%{_bindir}/btop
%{_bindir}/busers
%{_bindir}/lam-mpirun
%{_bindir}/mpich-mpirun
%{_bindir}/mpich2-mpiexec
%{_bindir}/openmpi-mpirun
%{_bindir}/lsacct
%{_bindir}/lseligible
%{_bindir}/lshosts
%{_bindir}/lsid
%{_bindir}/lsinfo
%{_bindir}/lsload
%{_bindir}/lsloadadj
%{_bindir}/lsmon
%{_bindir}/lsplace
%{_bindir}/lsrcp

# Man pages
%{_mandir}/man1/bbot.1
%{_mandir}/man1/bchkpnt.1
%{_mandir}/man1/bhosts.1
%{_mandir}/man1/bjobs.1
%{_mandir}/man1/bkill.1
%{_mandir}/man1/bmgroup.1
%{_mandir}/man1/bmig.1
%{_mandir}/man1/bmod.1
%{_mandir}/man1/bparams.1
%{_mandir}/man1/bpeek.1
%{_mandir}/man1/bqueues.1
%{_mandir}/man1/brequeue.1
%{_mandir}/man1/brestart.1
%{_mandir}/man1/bresume.1
%{_mandir}/man1/bstop.1
%{_mandir}/man1/bsub.1
%{_mandir}/man1/btop.1
%{_mandir}/man1/bugroup.1
%{_mandir}/man1/busers.1
%{_mandir}/man1/bswitch.1
%{_mandir}/man1/lsacct.1
%{_mandir}/man1/lseligible.1
%{_mandir}/man1/lsfbase.1
%{_mandir}/man1/lsfbatch.1
%{_mandir}/man1/lsfintro.1
%{_mandir}/man1/lshosts.1
%{_mandir}/man1/lsid.1
%{_mandir}/man1/lsinfo.1
%{_mandir}/man1/lsload.1
%{_mandir}/man1/lsloadadj.1
%{_mandir}/man1/lsmon.1
%{_mandir}/man1/lsplace.1
%{_mandir}/man1/lsrcp.1
%{_mandir}/man1/lstools.1
%{_mandir}/man5/lsb.acct.5
%{_mandir}/man5/lsb.events.5
%{_mandir}/man5/lsb.hosts.5
%{_mandir}/man5/lsb.params.5
%{_mandir}/man5/lsb.queues.5
%{_mandir}/man5/lsb.users.5
%{_mandir}/man8/badmin.8
%{_mandir}/man8/brun.8
%{_mandir}/man8/eauth.8
%{_mandir}/man8/eexec.8
%{_mandir}/man8/esub.8
%{_mandir}/man8/lim.8
%{_mandir}/man8/lsadmin.8
%{_mandir}/man8/lsfinstall.8
%{_mandir}/man8/mbatchd.8
%{_mandir}/man8/nios.8
%{_mandir}/man8/pim.8
%{_mandir}/man8/res.8
%{_mandir}/man8/sbatchd.8
%{_mandir}/man5/lim.acct.5
%{_mandir}/man5/lsf.acct.5
%{_mandir}/man5/lsf.cluster.5
%{_mandir}/man5/lsf.conf.5
%{_mandir}/man5/lsf.shared.5
%{_mandir}/man5/res.acct.5

# libraries
%{_libdir}/liblsf.a
%{_libdir}/liblsbatch.a

# headers
%{_includedir}/lsbatch.h
%{_includedir}/lsf.h

# docs
%doc COPYING

%defattr(0644,%{VOLCADMIN},%{VOLCADMIN})
%config(noreplace) %{_etcdir}/lsb.params
%config(noreplace) %{_etcdir}/lsb.queues
%config(noreplace) %{_etcdir}/lsb.hosts
%config(noreplace) %{_etcdir}/lsb.users
%config(noreplace) %{_etcdir}/lsf.shared
%config(noreplace) %{_etcdir}/lsf.conf
%config(noreplace) %{_etcdir}/lsf.cluster.%{CLUSTERNAME}
%config(noreplace) %{_etcdir}/lsf.task
%config(noreplace) %{_volclavatop}/README.md
%config(noreplace) %{_volclavatop}/COPYING
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_bindir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_etcdir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_includedir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_libdir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_logdir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_sbindir}
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/share
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/share/man
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/share/man/man1
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/share/man/man5
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/share/man/man8
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/work
%dir %attr(0755,%{VOLCADMIN},%{VOLCADMIN}) %{_volclavatop}/work/logdir

%changelog
* Sun Nov 16 2025 Releasing volclava 2.1.1 by Bytedance Ltd. and/or its affiliates
- support sorting hosts for job scheduling via order[slots];
- enhance mixed-OS deployment installation to maintain compatibility with single-platform mode;
- fix volclava script error on Ubuntu;
* Mon Sep 12 2025 Releasing volclava 2.1.0 by Bytedance Ltd. and/or its affiliates
- support adding comments to badmin operations;
- JOB_SPOOL_DIR supports the %U dynamic pattern format;
- support the customization of the admin and cluster name using the environment variables
  "volclavaadmin" and "volclavacluster" during the installation process;
- support mixed OS deployment in clusters installed via source code;
- modernize code for LIM compilation;
* Mon Jun 16 2025 Releasing volclava 2.0.0 by Bytedance Ltd. and/or its affiliates
- support fairshare scheduling policy for users at queue level;
- support customize unit by configure LSF_UNIT_FOR_LIMITS in lsf.conf;
- fix loadstop not effect job scheduling;
- fix memory leak in bhosts and putEnv();
- fix pim hang when format of pim.info file is not in volclava format;
- fix sbd hang due to unnecessary popen;
- fix lsload -w does not display full hostname;
- fix mbatchd coredump when only suspend jobs remain;
- fix mem be reserved repeatedly after bresume ususp/ssusp job;
- fix running jobs exceed the number of slots due to shared type resource when slotResourceReserve=y;
* Wed Feb 26 2025 Releasing volclava 1.0.2 by Bytedance Ltd. and/or its affiliates
- lsb.users: support MAX_PEND_JOBS and MAX_PEND_SLOTS;
- lsb.params: support MAX_PEND_JOBS and MAX_PEND_SLOTS and SUB_TRY_INTERVAL;
- busers: display MPEND as MAX_PEND_SLOTS defined in lsb.users, display PJOBS
- as statistic of user pend job, display MPJOBS as MAX_PEND_SLOTS defined in lsb.users;
- bparams: display MAX_PEND_JOBS and MAX_PEND_SLOTS and SUB_TRY_INTERVAL defined in lsb.params;
- bsub: add retry when submit job is limited by MAX_PEND_JOBS or MAX_PEND_SLOTS;
- support resource reserve as per-host;
* Mon Jan 06 2025 Releasing volclava 1.0.1 by Bytedance Ltd. and/or its affiliates
- bugfix: revert check for tcl result with function in tcl 8.6;
- configure: set default limit from cpu to cpu>=0 in lsf.task;
- update year of Copyright from 2021-2024 to 2021-2025;
- bsub: enhance parameter check of bsub -R;
- make source-script active in multiple volclava on the same host;
- make install script support ubuntu OS;
* Mon Nov 11 2024 Releasing volclava 1.0 by Bytedance Ltd. and/or its affiliates
- Multiple feature support: bjobs -UF; bjobs -o/-json; bsub -pack; bsub -Ep; etc.
- Multiple bugfix: MXJ not equal with maxCpus when set "!"; lshosts -l segmentation fault;
- sbatchd block by greater than 1000 jobs; prefix not work in rpm install to costomize directory;
- fix job slot limit reached while host is free.
- Define new project name as volclava in related files.
- Adapt to Ubuntu 20.04 and Rocky 8.10.
* Mon Jan 23 2012 Releasing openlava 2.0
* Sun Oct 30 2011 modified the spec file so that autoconf creates
- openlava configuration files and use the outptu variables to make
- the necessary subsititution in the them. Change the post install
- to just erase the package without saving anything.
- Removed the symbolic link as that is something sites have to
- do as they may want to run more versions together, also
- in now the lsf.conf has the version in the openlava
- fundamental variables clearly indicating which version is in use.
* Sun Sep 4 2011 David Bigagli restructured to follow the new directory layout after
the GNU autoconf project.
* Thu Jul 14 2011 Robert Stober <robert@openlava.net> 1.0-1
- Enhanced support for RPM uninstall. rpm -e openlava
- will now stop the openlava daemons and then completely
- remove openlava.
- openlava configuration files and log files are saved to
- /tmp/openlava.$$.tar.gz
- Uninstallation supports shared and non-shared file system
- installations
* Sat Jul 9 2011 Robert Stober <robert@openlava.net> 1.0-1
- Added the following files so that they're installed by the RPM:
- lsb.hosts
- openmpi-mpirun
- mpich-mpirun
- lam-mpirun
- mpich2-mpiexec
- The RPM installer now uses the template files that are in the
- scripts directory instead of the standard files that are installed
- by make:
- lsf.cluster.openlava
- lsf.conf
- lsf.shared
- openlava
- openlava.csh
- openlava.sh
* Thu Jun 16 2011 Robert Stober <robert@openlava.net> 1.0-1
- Changed name of openlava startup script from "lava" to "openlava"
- Changed the name of the linux service from "lava" to openlava in
- the openlava startup script
- Changed the name of the openlava shell initialization scripts
- from lava.sh and lava.csh to openlava.sh and openlava.csh respectively.
- Changed the openlava.spec file to install the README and COPYING files.
- Added the openlava.setup script, which streamlines openlava setup
- on compute servers.
* Sat Jun 11 2011 Robert Stober <robert@openlava.net> 1.0-1
- Changed default install directory to /opt/openlava-1.0
- Installation now creates a symbolic link openlava -> openlava-1.0
- RPM is now relocatable. Specify --prefix /path/to/install/dir
- for example, rpm -ivh --prefix /opt/test openlava-1.0-1.x86_64.rpm installs
- /opt/test/openlava -> /opt/test/openlava-1.0
- Added creation of openlava user
- Changed default cluster name to "openlava"
- Added support for cstomizing the cluster name
- For example, export OPENLAVA_CLUSTER_NAME="bokisius"
- then rpm -ivh openlava-1.0-1.x86_64.rpm this will:
- 1. Set the cluster name in the lsf.shared file
- 2. renames the "clustername" directories
- The LSF binaries are now statically linked instead of being
- dynamically linked.
- Renamed /etc/init.d/lava.sh to /etc/init.d/lava
- The openlava shell initialization files lava.sh and lava.csh
- are now installed in /etc/profile.d
* Fri Apr 22 2011 Robert Stober <rmstober@gmail.com> 1.0-6.6
- Changed to install in /opt/lava
- Added support for autoconfig of various lava config files
- Removed creation of openlava user
* Fri May 30 2008 Shawn Starr <sstarr@platform.com> 1.0-6
- Fix symlinks for MVAPICH1/2.
* Tue May 27 2008 Gerry Wen <gwen@platform.com> 1.0-2
- Add wrapper script for MPICH2 mpiexec
* Mon Feb 13 2008 Shawn Starr <sstarr@platform.com> 1.0-1
- Make home directory for openlava user.
* Mon Jan 23 2008 Shawn Starr <sstarr@platform.com> 1.0-0
- Initial release of Lava 1.0

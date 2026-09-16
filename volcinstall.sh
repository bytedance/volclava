#!/bin/bash
# Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates

if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

# Description:
# You can use this script in two ways to install volclava in shared file system:
# Way1: Install volclava in cluster by three steps:
#       1) Run 'volcinstall.sh --setup=pre' on each host in cluster.
#          This will help to setup environments and install some necessary packages
#          needed by volclava.
#       2) Run 'volcinstall.sh --setup=install --type=code --prefix=/share-nfs/software/volclava'
#          on master host.
#          This will install volclava package on the shared file system
#       3) Run 'volcinstall.sh --setup=post --env=/share-nfs/software/volclava'
#          on each host in cluster.
#          This will help enable the automatic startup of services and the automatic
#          addition of environment variables for each host in cluster.
# Way2: Install volclava in cluster by two steps:
#       1) Install master host: 'volcinstall.sh --type=code --prefix=/share-nfs/software/volclava'.
#          This will help finish all steps(pre/install/post) for master host.
#       2) Log on each compute node, run "volcinstall.sh --type=server --prefix=/share-nfs/software/volclava"
#          This will help finish installation steps on server host.
#
# Note:
# --hosts=/path/file: It is a file which lists hosts' name in one column. Default is not to add hosts into
#          lsf.cluster.${CLUSTERNAME}. If defined, installer will append hosts into lsf.cluster.${CLUSTERNAME}.
# --startup: Default is N. When set as Y, you also need define "--hosts", then we will startup cluster after
#          installation. Without "--hosts", volclava fails to startup cluster because of no hosts in cluster.
# --uid: specify uniform uid for user "volclava". Default is undefined.
# --file=/path/install.conf: Specify parameters related to cluster configuration in the file; the installer
#          will read them from the file. If parameters are defined both in the command line and the file,
#          the command line ones will take effect. The following is a list of parameters supported in the file.
#          For details of the parameters, please refer to the install.conf.example
#          VOLC_PREFIX
#          VOLC_ADMIN
#          VOLC_CLUSTER_NAME
#          VOLC_HOSTS
#          VOLC_MIX_OS_MODE
#          VOLC_MIX_OS_FOLDER
#          VOLC_BHIST_SPEEDUP

#######################################
# 1. initialize variables
#######################################
#Default values
CWD="$(dirname "$(realpath "$0")")"
source "${CWD}/libinstall.sh"

TYPE="code"
VERSION="2.2"
PACKAGE_NAME="volclava-${VERSION}"
VOLCADMIN="volclava"
CLUSTERNAME="volclava"
PREFIX="/opt/${PACKAGE_NAME}"
MIX_OS_MODE=0
MIX_OS_FOLDER="exec"
SET_PREFIX=0
PHASE="all"
USRID=""
HOSTS=""
STARTUP="N"
CONF_FILE_EXIST=0
CLS_FILE_EXIST=0
VOLC_SH_EXIT=0
VOLC_CSH_EXIT=0
ENABLE_BHIST_SPEEDUP="" # Resolve the default after loading configuration.

SCRIPT_PATH="$(realpath "$0")"

#######################################
# 2. define functions
#######################################
function usage() {
    echo "Usage: volcinstall.sh [--help]"
    echo "                      [--setup=pre [--uid=number] [--file=/path/install.conf] [--enable-bhist-speedup=Y|N]]"
    echo "                      [--setup=install [--type=code|rpm|deb] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file] [--file=/path/install.conf] [--enable-bhist-speedup=Y|N]]"
    echo "                      [--setup=post [--env=/volclava_top] [--startup=Y|y|N|n]]"
    echo "                      [--type=code|rpm|deb|server] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file] [--uid=number] [--startup=Y|y|N|n] [--file=/path/install.conf] [--enable-bhist-speedup=Y|N]"
}

function set_bhist_speedup_cli() {
    local requested=$1

    case "$requested" in
        Y|y)
            requested=Y
            ;;
        N|n)
            requested=N
            ;;
        *)
            echo "Error: --enable-bhist-speedup should be Y or N."
            usage
            exit 1
            ;;
    esac

    if [[ -n "$ENABLE_BHIST_SPEEDUP" && "$ENABLE_BHIST_SPEEDUP" != "$requested" ]]; then
        echo "Error: conflicting --enable-bhist-speedup values."
        usage
        exit 1
    fi

    ENABLE_BHIST_SPEEDUP=$requested
}

function configure_bhist_speedup_mode() {
    case "${ENABLE_BHIST_SPEEDUP:-${VOLC_BHIST_SPEEDUP:-N}}" in
        Y|y)
            ENABLE_BHIST_SPEEDUP=Y
            ;;
        N|n)
            ENABLE_BHIST_SPEEDUP=N
            ;;
        *)
            echo "Error: VOLC_BHIST_SPEEDUP should be Y, y, N or n."
            exit 1
            ;;
    esac
}

function get_installed_deb_prefix() {
    local env_file

    env_file=$(dpkg-query -L volclava 2>/dev/null \
        | sed -n '\#/etc/volclava\.sh$#p' \
        | head -n 1)
    if [ -z "$env_file" ]; then
        return 1
    fi

    dirname "$(dirname "$env_file")"
}

function pre_setup() {
    #add user
    groupadd -f ${VOLCADMIN} > /dev/null 2>&1 || true
    if [ -z "$USRID"  ]; then
        useradd -c "volclava Administrator" -g ${VOLCADMIN} -m -d /home/${VOLCADMIN} ${VOLCADMIN} > /dev/null 2>&1 || true
    else
        useradd -c "volclava Administrator" -u $USRID  -g ${VOLCADMIN} -m -d /home/${VOLCADMIN} ${VOLCADMIN} > /dev/null 2>&1 || true
    fi

    #install compile library
    if [ "$OS_NAME" = "rocky" ]; then
        yum install -y ncurses-devel tcl tcl-devel libtirpc libtirpc-devel libnsl2-devel
        if [[ $ENABLE_BHIST_SPEEDUP == Y ]]; then
            yum install -y sqlite-devel
        fi
        yum groupinstall -y "Development Tools"
    elif [ "$OS_NAME" = "ubuntu" ]; then
        apt update
        apt install -y build-essential automake tcl-dev libncurses-dev debhelper
        if [[ $ENABLE_BHIST_SPEEDUP == Y ]]; then
            apt install -y libsqlite3-dev
        fi
    else #CentOS
        yum install -y tcl-devel ncurses-devel
        if [[ $ENABLE_BHIST_SPEEDUP == Y ]]; then
            yum install -y sqlite-devel
        fi
        yum groupinstall -y "Development Tools"
    fi

    #close firewall
    if [ "$OS_NAME" != "ubuntu" ]; then 
        systemctl stop firewalld
        systemctl disable firewalld
    fi
}

function register_bhist_speedup_service() (
    # post/server installs reuse existing artifacts instead of a build flag.
    if [[ "$PHASE" != "post" && "$PHASE" != "pre-post" &&
          "$ENABLE_BHIST_SPEEDUP" != Y ]]; then
        return 0
    fi

    unset LSF_ENVDIR LSF_BINDIR
    source /etc/profile.d/volclava.sh || return 1
    if [[ "$LSF_ENVDIR" != /* || "$LSF_BINDIR" != /* ]] ||
        [ ! -d "$LSF_ENVDIR" ] || [ ! -d "$LSF_BINDIR" ]; then
        echo "Cannot register bhist-speedup: invalid LSF_ENVDIR or LSF_BINDIR." >&2
        return 1
    fi
    if [ ! -x "$LSF_ENVDIR/bhist-speedup" ] ||
        [ ! -x "$LSF_BINDIR/bhist-speedup-loader" ] ||
        [ ! -x "$LSF_BINDIR/bhist-speedup-server" ]; then
        if [[ "$PHASE" == "post" || "$PHASE" == "pre-post" ]]; then
            echo "Skipping bhist-speedup service registration: management script or binaries are not installed."
            return 0
        fi
        echo "Cannot register bhist-speedup: management script or binaries are missing." >&2
        return 1
    fi
    if ! command -v systemctl > /dev/null 2>&1 || [ ! -d /run/systemd/system ]; then
        echo "Skipping bhist-speedup service registration: systemd is not running."
        return 0
    fi

    # Stage both complete files in their destination directories before replacing.
    local timestamp script_tmp="" unit_tmp=""
    timestamp=$(date +%Y%m%d-%H%M%S) || return 1
    trap 'rm -f -- "$script_tmp" "$unit_tmp"' EXIT
    mkdir -p /etc/init.d /etc/systemd/system || return 1
    script_tmp=$(mktemp --suffix=".$timestamp" /etc/init.d/.bhist-speedup.script.XXXXXX) || return 1
    unit_tmp=$(mktemp --suffix=".$timestamp" /etc/systemd/system/.bhist-speedup.service.XXXXXX) || return 1
    command install -o root -g root -m 0755 "$LSF_ENVDIR/bhist-speedup" "$script_tmp" || return 1
    cat > "$unit_tmp" <<'EOF'
[Unit]
Description=bhist-speedup loader and server
After=network.target remote-fs.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/etc/init.d/bhist-speedup start
ExecStop=/etc/init.d/bhist-speedup stop
TimeoutStartSec=5min
TimeoutStopSec=1min
KillMode=control-group
Restart=no

[Install]
WantedBy=multi-user.target
EOF
    if [ "$?" -ne 0 ]; then
        return 1
    fi
    chown root:root "$unit_tmp" && chmod 0644 "$unit_tmp" || return 1
    mv -f "$script_tmp" /etc/init.d/bhist-speedup || return 1
    mv -f "$unit_tmp" /etc/systemd/system/bhist-speedup.service || return 1
    systemctl daemon-reload || return 1
    echo "bhist-speedup service registered. Start it with: systemctl start bhist-speedup"
    echo "To start it at boot: systemctl enable bhist-speedup"
)

function post_setup() {
    #rpm/deb way don't need post setup on master, they already setup service and shell environment

    if [[ "$PHASE" != "all" || ("$PHASE" == "all" && "$TYPE" == "code") ]]; then
        cp --backup=numbered $PREFIX/etc/volclava /etc/init.d/
        ln -sf $PREFIX/etc/volclava.csh /etc/profile.d/volclava.csh
        ln -sf $PREFIX/etc/volclava.sh /etc/profile.d/volclava.sh
    fi

    # configure the lava service to start at boot
    if [ "$OS_NAME" == "ubuntu" ]; then
       systemctl daemon-reload
       /lib/systemd/systemd-sysv-install enable volclava
    else
       chkconfig --add volclava
       chkconfig volclava on
    fi

    # startup cluster if need
    if [[ "$STARTUP" == "Y" ]] || [[ "$STARTUP" == "y" ]]; then
        service volclava start
    fi

    if ! register_bhist_speedup_service; then
        echo "Warning: bhist-speedup service registration is incomplete; software installation will continue." >&2
    fi

    source /etc/profile.d/volclava.sh

    if [ $? == 0 ]; then
        echo -e "Congratulates, installation is done and enjoy the journey!"
    fi
}

function addHosts2Cluster() {

    hostList=$1
    clusterFile=$2

    if [[ -z "$hostList" ]] || [[ -z "$clusterFile" ]]; then
        return 1
    fi

    if [ ! -e "$clusterFile" ]; then
        return 1
    fi

    if [[ "$hostList" == *"/"* ]]; then
        #from file
        if [ ! -f "$hostList" ];then
            echo "$hostList not exist, failed to add hosts to ${clusterFile}"
            return 1
        fi
        hostnames=($(cat $hostList))
    else
        hostnames=($(echo $hostList))
    fi

    found_place=false
    line_number=0
    while read line; do
        ((line_number++))
        trimmed_line=$(echo "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        if [[ "$trimmed_line" == HOSTNAME* ]]; then
            found_place=true
        fi

        if [[ "$found_place" == true ]]; then
            for hostname in "${hostnames[@]}"; do
                sed -i "${line_number}a\\${hostname}           IntelI5      linux   1      3.5    (cs)" $clusterFile
                ((line_number++)) 
            done
            break
        fi
    done < $clusterFile
    return 0
}


function install() {

    if [ "$TYPE" = "code" ]; then
        # install volclava from source code
	cd ${CWD}
        #setup automake
        if [[ $MIX_OS_MODE == 1  ]]; then
            #install in multi-platform mode
            platform=${OS_NAME}-${OS_VERSION}-${CPU_ARCH}
            ./bootstrap.sh --prefix=$PREFIX --exec-prefix=${PREFIX}/${MIX_OS_FOLDER}/${platform} \
                "--enable-bhist-speedup=$ENABLE_BHIST_SPEEDUP"
        else
            #install in single platform mode
            ./bootstrap.sh --prefix=$PREFIX "--enable-bhist-speedup=$ENABLE_BHIST_SPEEDUP"
        fi

        #make and install
        if [ $? -eq 0 ]; then
            make
        else
            echo "Failed to setup autoconf and automake ..."
            exit 1 
        fi
        if [ $? -eq 0 ]; then
            make install
        else
            echo "Failed to execute make ..."
            exit 1
        fi

        #After build install, let us modify configuration according to user specification
        chown ${VOLCADMIN}:${VOLCADMIN} -R $PREFIX
        chmod 755 -R $PREFIX

        #append hosts into lsf.cluster file
        if [[ $CLS_FILE_EXIST == 0 && -n "$HOSTS" ]]; then
            addHosts2Cluster "$HOSTS" ${PREFIX}/etc/lsf.cluster.${CLUSTERNAME}
        fi
        #modify volclava.sh,volclava.csh,lsf.conf for multi-platform mode
        if [[ $MIX_OS_MODE == 1 ]]; then
            if [[ $VOLC_SH_EXIT == 0 ]]; then
                sed -i "s/^MIX_OS_FOLDER=.*/MIX_OS_FOLDER=$MIX_OS_FOLDER/" ${PREFIX}/etc/volclava.sh
            fi
            if [[ $VOLC_CSH_EXIT == 0  ]];then
                sed -i "s/^set MIX_OS_FOLDER=.*/set MIX_OS_FOLDER=$MIX_OS_FOLDER/" ${PREFIX}/etc/volclava.csh
            fi
            if [[ $CONF_FILE_EXIST == 0 ]]; then
                sed -i '/^LSF_SERVERDIR=/d' ${PREFIX}/etc/lsf.conf
                sed -i '/^LSF_BINDIR=/d' ${PREFIX}/etc/lsf.conf
            fi
        fi
    elif [ "$TYPE" = "deb" ]; then
        #deb way to install volclava
        chmod 755 bootstrap.sh

        installed_deb_status=$(dpkg-query -W -f='${db:Status-Abbrev}' \
            volclava 2>/dev/null || true)
        if [[ "$installed_deb_status" == ii* ]]; then
            installed_deb_prefix=$(get_installed_deb_prefix || true)
            if [ -z "$installed_deb_prefix" ]; then
                echo "Failed to determine the prefix of the installed volclava package."
                exit 1
            fi
            if [ "$installed_deb_prefix" != "$PREFIX" ]; then
                echo "The installed volclava package uses prefix $installed_deb_prefix."
                echo "Changing the prefix during a DEB upgrade is not supported."
                exit 1
            fi
        fi

        #create deb under ../
        deb_build_profiles=""
        for profile in ${DEB_BUILD_PROFILES:-}; do
            if [[ "$profile" != "pkg.volclava.bhist-speedup" ]]; then
                deb_build_profiles="${deb_build_profiles:+${deb_build_profiles} }${profile}"
            fi
        done
        if [[ $ENABLE_BHIST_SPEEDUP == Y ]]; then
            deb_build_profiles="${deb_build_profiles:+${deb_build_profiles} }pkg.volclava.bhist-speedup"
        fi
        DEB_BUILD_PROFILES="$deb_build_profiles" \
            VOLC_BHIST_SPEEDUP="$ENABLE_BHIST_SPEEDUP" \
            VOLC_PREFIX="$PREFIX" \
            dpkg-buildpackage -b -rfakeroot -us -uc
        if [ $? -ne 0 ]; then
            echo "Failed to create volclava deb package. Please check."
            exit 1
        fi

        # Install through dpkg so dependencies, maintainer scripts, upgrades,
        # conffiles and uninstall remain under Debian package management.
        dpkg -i ../volclava_2.2*.deb
        if [ $? -ne 0 ]; then
            echo "Failed to install volclava deb package. Please check."
            exit 1
        fi

        #append hosts into lsf.cluster file
        if [[ $CLS_FILE_EXIST == 0 && -n "$HOSTS" ]]; then
             addHosts2Cluster "$HOSTS" ${PREFIX}/etc/lsf.cluster.${CLUSTERNAME}
        fi
    else
        #rpm way to install volclava
        if which yum > /dev/null 2>&1; then
            if ! yum list installed rpm-build >/dev/null 2>&1; then
                yum install -y rpm-build
            fi
            if ! yum list installed rpmdevtools >/dev/null 2>&1; then
                yum install -y rpmdevtools
            fi
        else
            echo "Failed to find yum ..."
            exit 1
        fi

        chmod 755 rpm.sh
        chmod 755 bootstrap.sh

        #create rpm under ~/rpmbuild/RPMS/x86-64
        ./rpm.sh "--enable-bhist-speedup=$ENABLE_BHIST_SPEEDUP"
        if [ $? -ne 0 ]; then
            echo "Failed to create volclava rpm package. Please check."
            exit 1
        fi

        #install volclava from rpm package
        cd ~/rpmbuild/RPMS/x86_64/
        chmod 755 volclava-2.2*
        if rpm -qa | grep volclava-2.2* > /dev/null 2>&1; then
            rpm -e volclava-2.2*
        fi
        rpm -ivh --prefix $PREFIX volclava-2.2*

        #append hosts into lsf.cluster file
        if [[ $CLS_FILE_EXIST == 0 && -n "$HOSTS" ]]; then
             addHosts2Cluster  "$HOSTS" ${PREFIX}/${PACKAGE_NAME}/etc/lsf.cluster.${CLUSTERNAME}
        fi
    fi
}

while [ $# -gt 0 ]; do
    case $1 in
        --type=*)
            TYPE=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$TYPE" ]; then
                echo "Error: the value of \"--type\" is empty."
                usage
                exit 1
            fi
            if [ "$TYPE" == "rpm" -a $SET_PREFIX -eq 0 ]; then
                PREFIX="/opt"
            fi
            if [ "$TYPE" == "server" -a $PHASE != "all" ]; then
                echo "Error: --type=server cannot be used with --setup."
                usage
                exit 1
            fi
            if [ "$TYPE" == "server" ]; then
                PHASE="pre-post"
            fi
            ;;
        --prefix=*)
            PREFIX=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$PREFIX" ]; then
                echo "Error: the value of \"--prefix\" is empty."
                usage
                exit 1
            fi
            SET_PREFIX=1
            ;;
        --setup=*)
            PHASE=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$PHASE" ]; then
                echo "Error: the value of \"--setup\" is empty."
                usage
                exit 1
            fi
	    if [ "$PHASE" != "pre" -a "$PHASE" != "post" -a "$PHASE" != "install" ]; then
                echo "Error: The value of \"--setup\" should be 'pre', 'post' or 'install'."
		usage
	        exit 1
	    fi	
	    ;;
        --env=*)
            PREFIX=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$PREFIX" ]; then
                echo "Error: the value of \"--env\" is empty."
                usage
                exit 1
            fi
            ;;
        --uid=*)
            USRID=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$USRID" ]; then
                echo "Error: the value of \"--uid\" is empty."
                usage
                exit 1
            fi
            ;;
        --hosts=*)
            HOSTS=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$HOSTS" ]; then
                echo "Error: the value of \"--hosts\" is empty."
                usage
                exit 1
            fi
            ;;
        --startup=*)
            STARTUP=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$STARTUP" ]; then
                echo "Error: the value of \"--startup\" is empty."
                usage
                exit 1
            fi
            if [[ "$STARTUP" != "Y" ]] && [[ "$STARTUP" != "y" ]] && [[ "$STARTUP" != "N" ]] && [[ "$STARTUP" != "n" ]]; then
                echo "Error: the value of \"--startup\" should be y|Y|n|N."
                usage
                exit 1
            fi
            ;;
        --file=*)
            INSTALL_CONF_FILE=$(echo $1 | awk -F "=" '{print $2}')
            if [[ -z "$INSTALL_CONF_FILE" || ! -e "$INSTALL_CONF_FILE" || -d "$INSTALL_CONF_FILE" ]]; then
                echo "Error: the value of \"--file\" should be an existing file with a path."
                usage
                exit 1
            fi
            ;;
        --enable-bhist-speedup=*)
            set_bhist_speedup_cli "${1#*=}"
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
    shift
done


#######################################
# 3. main logic
#######################################
# Validate options
if [[ "$TYPE" != "code" && "$TYPE" != "rpm" && "$TYPE" != "deb" && "$TYPE" != "server" ]]; then
    echo "Error: the value of \"--type\" is invalid."
    usage
    exit 1
fi

if [[ ("$TYPE" == "server" &&  "$PHASE" != "pre-post") || ("$TYPE" != "server" && "$PHASE" == "pre-post") ]]; then
    echo "Error: --type=server cannot be used with --setup."
    usage
    exit 1
fi

read OS_NAME OS_VERSION CPU_ARCH <<< "$(get_os_info)"
if [ "$OS_NAME" == "unknown" ]; then
    echo "Failed to find OS type, please check supported OS from README.md"
    exit 1
fi

if [ "$OS_NAME" = "ubuntu" ] && [ "$TYPE" = "rpm" ]; then
   echo "Ubuntu does not support rpm installation, please install package from source code"
   exit 1
fi

if [ "$OS_NAME" = "rocky" ] && [ "$TYPE" = "deb" ]; then
   echo "Rocky Linux does not support deb installation, please install package from source code"
   exit 1
fi

if [ "$OS_NAME" = "centos" ] && [ "$TYPE" = "deb" ]; then
   echo "CentOS Linux does not support deb installation, please install package from source code"
   exit 1
fi

if [ "$OS_NAME" = "redhat" ] && [ "$TYPE" = "deb" ]; then
   echo "Redhat Linux does not support deb installation, please install package from source code"
   exit 1
fi

#Initialization configration from file or env, order of configuration effectiveness as following:
#obvious options -> config from specified file -> environment if set -> default value
if [ -n "$INSTALL_CONF_FILE" ];then
    . $INSTALL_CONF_FILE
fi

# The post-only phase does not build software or install build dependencies.
if [[ "$PHASE" != "post" ]]; then
    configure_bhist_speedup_mode
fi

#PREFIX
if [[ "$SET_PREFIX" == 0 && -n "$VOLC_PREFIX" ]]; then
    PREFIX=$VOLC_PREFIX
    SET_PREFIX=1
fi
#VOLCADMIN
if [ -n "$VOLC_ADMIN" ]; then
    VOLCADMIN=$VOLC_ADMIN
    export volclavaadmin=$VOLCADMIN
elif [ -n "$volclavaadmin" ]; then   
    VOLCADMIN=$volclavaadmin
fi
#CLUSTERNAME
if [ -n "$VOLC_CLUSTER_NAME" ]; then
    CLUSTERNAME=$VOLC_CLUSTER_NAME
    export volclavacluster=$CLUSTERNAME
elif [ -n "$volclavacluster" ]; then 
    CLUSTERNAME=$volclavacluster
fi
#HOSTS
if [[ -z "$HOSTS" && -n "$VOLC_HOSTS" ]]; then
    HOSTS=$VOLC_HOSTS
fi
#MIX_OS_MODE
if [[ "$VOLC_MIX_OS_MODE" == "Y" || "$VOLC_MIX_OS_MODE" == "y" ]]; then
    MIX_OS_MODE=1
fi

if [[ "$VOLC_MIX_OS_MODE" == "Y" && ("$TYPE" == "rpm" || "$TYPE" == "deb") ]]; then
    echo "Volclava does not support multi-platform installation via RPM or DEB packages. Please set VOLC_MIX_OS_MODE=N in install.conf and try again."
    exit 1    
fi

if [[ "$TYPE" == "deb" ]]; then
    if [[ "$PREFIX" != /* ]]; then
        echo "A DEB installation prefix must be an absolute path: $PREFIX"
        exit 1
    fi
    if [[ "$PREFIX" == "/" ]]; then
        echo "The filesystem root cannot be used as a DEB installation prefix."
        exit 1
    fi
    if [[ "$PREFIX" =~ [[:space:]] ]]; then
        echo "A DEB installation prefix must not contain whitespace: $PREFIX"
        exit 1
    fi
    while [[ "$PREFIX" != "/" && "$PREFIX" == */ ]]; do
        PREFIX=${PREFIX%/}
    done
fi

#MIX_OS_FOLDER
if [ -n "$VOLC_MIX_OS_FOLDER" ]; then
    MIX_OS_FOLDER=$VOLC_MIX_OS_FOLDER
fi

#Other internal variables
if [[ -e $PREFIX/etc/lsf.conf ]]; then
   CONF_FILE_EXIST=1
fi
if [[ -e $PREFIX/etc/lsf.cluster.${CLUSTERNAME} ]]; then
   CLS_FILE_EXIST=1
fi
if [[ -e $PREFIX/etc/volclava.sh ]]; then
   VOLC_SH_EXIST=1
fi
if [[ -e $PREFIX/etc/volclava.csh ]]; then
   VOLC_CSH_EXIST=1
fi

#Execute installation
if [ "$PHASE" == "pre" ]; then
    pre_setup
    echo "Preparation is done. You can use \"volcinstall.sh --setup=install ...\" to install the package"
elif [ "$PHASE" == "install" ]; then
    install
    if [[ "$TYPE" == "code" || "$TYPE" == "deb" ]]; then
        echo -e "The volclava is installed under ${PREFIX}\nYou can use the following command to enable services to startup and add environment variables automatically on master and computing nodes: \n$0 --setup=post --env=${PREFIX}"
    else
        echo -e "The volclava is installed under ${PREFIX}/${PACKAGE_NAME}\nYou can use the following command to enable services to startup and add environment variables automatically on other computing nodes:\n$0 --setup=post --env=${PREFIX}/${PACKAGE_NAME}"
    fi
elif [ "$PHASE" == "post" ]; then
    post_setup
elif [ "$PHASE" == "pre-post" ]; then
    pre_setup
    post_setup
elif [ "$PHASE" == "all" ]; then
    pre_setup
    install
    post_setup
else
    echo "Failed to install volclava ..."
    exit 1
fi

exit 0

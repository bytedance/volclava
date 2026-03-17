#!/bin/bash
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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

#######################################
# 1. initialize variables
#######################################
#Default values
source ./libinstall.sh

TYPE="code"
VERSION="2.1"
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

SCRIPT_PATH="$(realpath "$0")"
CWD="$(dirname "$(realpath "$0")")"

#######################################
# 2. define functions
#######################################
function usage() {
    echo "Usage: volcinstall.sh [--help]"
    echo "                      [--setup=pre [--uid=number]]"
    echo "                      [--setup=install [--type=code|rpm|deb] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file] [--file=/path/install.conf]]"
    echo "                      [--setup=post [--env=/volclava_top] [--startup=Y|y|N|n]]"
    echo "                      [--type=code|rpm|deb|server] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file] [--uid=number] [--startup=Y|y|N|n] [--file=/path/install.conf]"
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
        yum groupinstall -y "Development Tools"
    elif [ "$OS_NAME" = "ubuntu" ]; then
        apt update
        apt install -y build-essential automake tcl-dev libncurses-dev debhelper
    else #CentOS
        yum install -y tcl-devel ncurses-devel
        yum groupinstall -y "Development Tools"
    fi

    #close firewall
    if [ "$OS_NAME" != "ubuntu" ]; then 
        systemctl stop firewalld
        systemctl disable firewalld
    fi
}

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
            ./bootstrap.sh --prefix=$PREFIX --exec-prefix=${PREFIX}/${MIX_OS_FOLDER}/${platform}
        else
            #install in single platform mode
            ./bootstrap.sh --prefix=$PREFIX
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

        #create deb under ../
        dpkg-buildpackage -b -rfakeroot -us -uc
        if [ $? -ne 0 ]; then
            echo "Failed to create volclava deb package. Please check."
            exit 1
        fi

        #Remove old volclava from deb package
        if dpkg -l | grep volclava > /dev/null 2>&1; then
            dpkg -P volclava
        fi

        if [ $SET_PREFIX -ne 0 ]; then
            #install deb with prefix
            dpkg -x ../volclava_2.1*.deb $PREFIX
            #append hosts into lsf.cluster file
            if [ ! -z "$HOSTS" ]; then
                 addHosts2Cluster  "$HOSTS" ${PREFIX}/opt/${PACKAGE_NAME}/etc/lsf.cluster.${CLUSTERNAME}
            fi
            sed -i "s|/opt/${PACKAGE_NAME}|${PREFIX}/opt/${PACKAGE_NAME}|g" $(grep -rl /opt/${PACKAGE_NAME} ${PREFIX}/)

            chown ${VOLCADMIN}:${VOLCADMIN}  -R $PREFIX
            chmod 755 -R $PREFIX
        else
            dpkg -i ../volclava_2.1*.deb
            #append hosts into lsf.cluster file
            if [[  $CLS_FILE_EXIST == 0 && -n "$HOSTS" ]]; then
                 addHosts2Cluster  "$HOSTS" /opt/${PACKAGE_NAME}/etc/lsf.cluster.${CLUSTERNAME}
            fi
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
        ./rpm.sh
        if [ $? -ne 0 ]; then
            echo "Failed to create volclava rpm package. Please check."
            exit 1
        fi

        #install volclava from rpm package
        cd ~/rpmbuild/RPMS/x86_64/
        chmod 755 volclava-2.1*
        if rpm -qa | grep volclava-2.1* > /dev/null 2>&1; then
            rpm -e volclava-2.1*
        fi
        rpm -ivh --prefix $PREFIX volclava-2.1*

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
            if [ "$TYPE" == "deb" -a $SET_PREFIX -eq 0 ]; then
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

if [[ "$TYPE" == "deb" && $SET_PREFIX == 1 ]]; then
    echo "Currently, installing Volclava via deb package does not support specifying a custom prefix. Please check the prefix settings in the command-line arguments or installation config file."
    exit 1
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
    if [ "$TYPE" == "code" ]; then
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

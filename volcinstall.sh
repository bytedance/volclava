#!/bin/bash

# Copyright (C) 2021-2024 Bytedance Ltd. and/or its affiliates
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
# --hosts=/path/file: It is a file which lists hosts' name in one column.
# --startup: default is N. If set as Y, we will startup cluster after installation

function usage() {
    echo "Usage: volcinstall.sh [--help]"
    echo "                      [--setup=pre [--uid=number]]"
    echo "                      [--setup=install [--type=code|rpm] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file]]"
    echo "                      [--setup=post [--env=/volclava_top] --startup=Y|y|N|n]"
    echo "                      [--type=code|rpm|server] [--prefix=/opt/volclava] [--hosts=\"master server1 ...\"|/path/file]"
}


#Default values
TYPE="code"
PACKAGE_NAME="volclava-1.0"
PREFIX="/opt/${PACKAGE_NAME}"
setPrefix=0
PHASE="all"
USRID=""
HOSTS=""
STARTUP="N"

while [ $# -gt 0 ]; do
    case $1 in
        --type=*)
            TYPE=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z $TYPE ]; then
                usage
                exit 1
            fi
            if [ $TYPE == "rpm" -a $setPrefix -eq 0 ]; then
                PREFIX="/opt"
            fi
            if [ $TYPE == "server" -a $PHASE != "all" ]; then
                usage
                exit 1
            fi
            if [ $TYPE == "server" ]; then
                PHASE="pre-post"
            fi
            ;;
        --prefix=*)
            PREFIX=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z $PREFIX ]; then
                usage
                exit 1
            fi
            setPrefix=1
            ;;
        --setup=*)
            PHASE=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z $PHASE ]; then
                usage
                exit 1
            fi
	    if [ $PHASE != "pre" -a $PHASE != "post" -a $PHASE != "install" ]; then
		usage
	        exit 1
	    fi	
	    ;;
        --env=*)
            PREFIX=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z $PREFIX ]; then
                usage
                exit 1
            fi
            ;;
        --uid=*)
            USRID=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z $USRID ]; then
                usage
                exit 1
            fi
            ;;
        --hosts=*)
            HOSTS=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$HOSTS" ]; then
                usage
                exit 1
            fi
            ;;
        --startup=*)
            STARTUP=$(echo $1 | awk -F "=" '{print $2}')
            if [ -z "$STARTUP" ]; then
                usage
                exit 1
            fi
            if [[ "$STARTUP" != "Y" ]] && [[ "$STARTUP" != "y" ]] && [[ "$STARTUP" != "N" ]] && [[ "$STARTUP" != "n" ]]; then
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

if [ $TYPE != "code" -a $TYPE != "rpm" -a $TYPE != "server" ]; then
    usage
    exit 1
fi

if [ $TYPE = "server" -a  $PHASE != "pre-post" ] || [ $TYPE != "server" -a $PHASE = "pre-post" ]; then
    usage
    exit 1
fi

function pre_setup() {
    #add user
    if ! id -u "volclava" > /dev/null 2>&1; then
        if [ -z "$USRID"  ]; then
            useradd "volclava"
        else
            useradd -u $USRID  "volclava" 
        fi
    fi

    #install compile library
    osType=$(cat /etc/redhat-release | awk 'NR==1{print $1}')

    if [ $osType = "Rocky" ]; then
        yum install -y ncurses-devel tcl tcl-devel libtirpc libtirpc-devel libnsl2-devel    
        yum groupinstall -y "Development Tools"
    else
        yum install -y tcl-devel ncurses-devel
        yum groupinstall -y "Development Tools"
    fi

    #close firewall
    systemctl stop firewalld
    systemctl disable firewalld
}

function post_setup() {
    #rpm way doesn't need post setup, rpm already setup service and shell environment
    if [ $TYPE = "rpm" -a $PHASE = "all" ]; then
        return
    fi

    #set up volclava service and shell environment 
    cp -f $PREFIX/etc/volclava /etc/init.d/
    cp -f $PREFIX/etc/volclava.csh /etc/profile.d/
    cp -f $PREFIX/etc/volclava.sh /etc/profile.d/

    # configure the lava service to start at boot
    chkconfig --add volclava
    chkconfig volclava on

    # startup cluster if need
    if [[ $STARTUP == "Y" ]] || [[ $STARTUP == "y" ]]; then
        service volclava start
    fi

    source ${PREFIX}/etc/volclava.sh

    if [ $? == 0 ]; then
        echo -e "Congratulates, installation is done and enjoy the journey!"
    fi
}

function addHosts2Cluster() {

    hostList=$1
    clusterFile=$2

    if [[ -z $hostList ]] || [[ -z $clusterFile ]]; then
        return 1
    fi

    if [ ! -e "$clusterFile" ]; then
        return 1
    fi

    if [[ $hostList == *"/"* ]]; then
        #from file
        if [ ! -f $hostList ];then
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
        if [[ $trimmed_line == HOSTNAME* ]]; then
            found_place=true
        fi

        if [ "$found_place" == true ]; then
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

    if [ $TYPE = "code" ]; then
        # install volclava from source code
        #setup automake
        ./bootstrap.sh --prefix=$PREFIX

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

        chown volclava:volclava -R $PREFIX
        chmod 755 -R $PREFIX

        #append hosts into lsf.cluster file
        if [ ! -z "$HOSTS" ]; then
            addHosts2Cluster "$HOSTS" ${PREFIX}/etc/lsf.cluster.volclava
        fi
    else
        #rpm way to intall volclava 
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
        chmod 755 volclava-1.0*
        if rpm -qa | grep volclava-1.0* > /dev/null 2>&1; then
            rpm -e volclava-1.0*
        fi
        rpm -ivh --prefix $PREFIX volclava-1.0*

        #append hosts into lsf.cluster file
        if [ ! -z "$HOSTS" ]; then
             addHosts2Cluster  "$HOSTS" ${PREFIX}/${PACKAGE_NAME}/etc/lsf.cluster.volclava
        fi
    fi
}

if [ $PHASE == "pre" ]; then
    pre_setup
    echo "Preparation is done. You can use \"volcinstall.sh --setup=install ...\" to install the package"
elif [ $PHASE == "install" ]; then
    install
    if [ "$TYPE" == "code" ]; then
        echo -e "The volclava is installed under ${PREFIX}\nYou can use the following command to enable services to startup and add environment variables automatically on master and computing nodes: \n$0 --setup=post --env=${PREFIX}"
    else
        echo -e "The volclava is installed under ${PREFIX}/${PACKAGE_NAME}\nYou can use the following command to enable services to startup and add environment variables automatically other computing nodes:\n$0 --setup=post --env=${PREFIX}/${PACKAGE_NAME}"
    fi
elif [ $PHASE == "post" ]; then
    post_setup
elif [ $PHASE == "pre-post" ]; then
    pre_setup
    post_setup
elif [ $PHASE == "all" ]; then
    pre_setup
    install
    post_setup
else
    echo "Failed to install volclava ..."
    exit 1
fi

exit 0
#!/bin/bash

# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates

VERSION="2.1"

if [ $# -ne 1 ]; then
    echo "Usage: volcuninstall.sh /volclava_top"
    exit 1
fi

if [ ! -e "$1" ]; then
    echo "$1 not exsit"
    exit 1
fi

if [ ! -d "$1" ]; then
    echo "$1 is not directory"
    exit 1
fi

# Get plaform
OS_NAME="unknown"
OS_VERSION="unknown"
CPU_ARCH="unknown"

# Check OS name and version
if [ -f /etc/os-release ]; then
    . /etc/os-release
    
    #check ubuntu
    if [ "$ID" = "ubuntu" ] || [ "$ID_LIKE" = "debian" ]; then
        OS_NAME="ubuntu"
        OS_VERSION=$VERSION_ID
    
    #check centos
    elif [ "$ID" = "centos" ]; then
        OS_NAME="centos"
        OS_VERSION=$VERSION_ID
    
    #check rocky
    elif [ "$ID" = "rocky" ]; then
        OS_NAME="rocky"
        OS_VERSION=$VERSION_ID
    
    #check redhat
    elif [ "$ID" = "rhel" ] || [ "$ID_LIKE" = "rhel" ] || [ "$NAME" = "Red Hat Enterprise Linux" ]; then
        OS_NAME="redhat"
        OS_VERSION=$VERSION_ID
    fi

# Compatible with older systems without /etc/os-release
else
    if [ -f /etc/redhat-release ]; then
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
    elif [ -f /etc/lsb-release ]; then
        . /etc/lsb-release
        if [ "$DISTRIB_ID" = "Ubuntu" ]; then
            OS_NAME="ubuntu"
            OS_VERSION=$DISTRIB_RELEASE
        fi
    fi
fi

# Check main version
OS_VERSION=$(echo "$OS_VERSION" | sed -E 's/[^0-9.]//g' | cut -d '.' -f 1)

# Check CPU arch
CPU_ARCH=$(uname -m)
PLATFORM="${OS_NAME}-${OS_VERSION}-${CPU_ARCH}"

service volclava stop

VOLC_TOP=$(echo "$1" | sed 's/\/$//')
BINARY_PATH="${VOLC_TOP}/exec/${PLATFORM}"

DAEMON_PIDS=$(ps -ef | grep "${BINARY_PATH}/sbin" | grep -v grep | awk '{print $2}')
if [ ! -z "$DAEMON_PIDS" ]; then
    ps -ef | grep "$new_dir_path/sbin" | grep -v grep | awk '{print $2}' | xargs kill -9
fi

if [ "${OS_NAME}" = "ubuntu" ]; then
    /lib/systemd/systemd-sysv-install disable volclava

    if dpkg -l | grep volclava > /dev/null 2>&1; then
        dpkg -P volclava
        echo "Volclava has been successfully uninstalled."
        exit 0
    fi
else
    chkconfig volclava off > /dev/null 2>&1
    chkconfig --del volclava > /dev/null 2>&1

    if rpm -qa | grep volclava-${VERSION}* > /dev/null 2>&1; then
       rpm -e volclava-${VERSION}*
       echo "Volclava has been successfully uninstalled."
       exit 0
    fi
fi

# Uninstall for installing from source code
if [ -d "${BINARY_PATH}" ]; then
    rm -rf ${BINARY_PATH} || true
fi
rm -f /etc/init.d/volclava* > /dev/null 2>&1 || true
rm -f /etc/profile.d/volclava.* > /dev/null 2>&1 || true

DIR_COUNT=$(find "${VOLC_TOP}/exec" -maxdepth 1 -mindepth 1 | wc -l)
if [ "$DIR_COUNT" -eq 0 ]; then
    rm -rf "${VOLC_TOP}/exec" "${VOLC_TOP}/share" "${VOLC_TOP}/include" /dev/null 2>&1   || true
    echo "Volclava has been successfully uninstalled. Please manually delete the remaining application data."
else
    echo "Volclava has been successfully uninstalled from the ${PLATFORM} platform."
fi
exit 0

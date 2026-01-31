#!/bin/bash

# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates

source ./libinstall.sh

function usage() {
    echo "Usage: volcuninstall.sh [--help]"
    echo "                        [--env=/volclava_top]"
}


VERSION="2.1"
VOLC_TOP=""
INSTALL_CONF_FILE=""

while [ $# -gt 0 ]; do
    case $1 in
        --env=*)
            VOLC_TOP=$(echo "$1" | awk -F "=" '{print $2}' | sed 's/\/$//')
            if [[ -z $VOLC_TOP ]];then
                "Error: the value of \"--env\" is empty."
                usage
                exit 1
            fi
            if [ ! -d $VOLC_TOP ]; then
                echo "Error: $VOLC_TOP is not directory."
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

if [[ -z "$VOLC_TOP" &&  -z "$LSF_ENVDIR" ]];then
    echo "Error: Please specify --env=/volclava_top or source volclava environments"
    usage
    exit 1
fi

if [[ -z "$VOLC_TOP" ]];then
    VOLC_TOP=$(dirname $LSF_ENVDIR)
fi

if [ -e $LSF_ENVDIR/volclava.sh ]; then
    MIX_OS_FOLDER=$(grep '^MIX_OS_FOLDER=' $LSF_ENVDIR/volclava.sh  | tail -n 1 | cut -d'=' -f2- | tr -d '"')
else
    echo "Cannot find $LSF_ENVDIR/volclava.sh. We cannot determine the current installation mode, exit..."
    exit 1
fi

# Get platform
read OS_NAME OS_VERSION CPU_ARCH <<< $(get_os_info)
if [[ -n ${MIX_OS_FOLDER} ]]; then
    PLATFORM="${OS_NAME}-${OS_VERSION}-${CPU_ARCH}"
    BINARY_PATH="${VOLC_TOP}/${MIX_OS_FOLDER}/${PLATFORM}"
else
    BINARY_PATH=$VOLC_TOP
fi

service volclava stop  > /dev/null 2>&1
DAEMON_PIDS=$(ps -ef | grep "${BINARY_PATH}/sbin" | grep -v grep | awk '{print $2}')
if [ -n "$DAEMON_PIDS" ]; then
    ps -ef | grep "${BINARY_PATH}/sbin" | grep -v grep | awk '{print $2}' | xargs kill -9
fi

if [ "${OS_NAME}" = "ubuntu" ]; then
    /lib/systemd/systemd-sysv-install disable volclava  > /dev/null 2>&1

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
if [[ -n ${MIX_OS_FOLDER} ]]; then
    if [ -d "${BINARY_PATH}" ]; then
        rm -rf ${BINARY_PATH} || true
    fi
else
    rm -rf ${BINARY_PATH}/bin ${BINARY_PATH}/sbin ${BINARY_PATH}/lib ${VOLC_TOP}/share ${VOLC_TOP}/include || true
fi

rm -f /etc/init.d/volclava* > /dev/null 2>&1 || true
rm -f /etc/profile.d/volclava.* > /dev/null 2>&1 || true
systemctl daemon-reload > /dev/null 2>&1 || true

if [[ -n ${MIX_OS_FOLDER} ]]; then
    DIR_COUNT=$(find "${VOLC_TOP}/${MIX_OS_FOLDER}" -maxdepth 1 -mindepth 1 | wc -l)
    if [ "$DIR_COUNT" -eq 0 ]; then
        rm -rf "${VOLC_TOP}/${MIX_OS_FOLDER}" "${VOLC_TOP}/share" "${VOLC_TOP}/include" /dev/null 2>&1   || true
        echo "Volclava has been successfully uninstalled. Please manually delete the remaining application data."
    else
        echo "Volclava has been successfully uninstalled from the ${PLATFORM} platform."
    fi
else
    echo "Volclava has been successfully uninstalled. Please manually delete the remaining application data."
fi

exit 0

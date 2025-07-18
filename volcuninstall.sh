#!/bin/bash

# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates

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

service volclava stop

new_dir_path=$(echo "$1" | sed 's/\/$//')
daemon_pids=$(ps -ef | grep "$new_dir_path/sbin" | grep -v grep | awk '{print $2}')
if [ ! -z "$daemon_pids" ]; then
    ps -ef | grep "$new_dir_path/sbin" | grep -v grep | awk '{print $2}' | xargs kill -9
fi

osType=$(sed -n '/^NAME=/ {s/^NAME="//;s/"$//;p}' /etc/os-release)
if [ "$osType" == "Ubuntu" ]; then
    /lib/systemd/systemd-sysv-install disable volclava
    if dpkg -l | grep volclava > /dev/null 2>&1; then
        dpkg -P volclava
    fi
else
    chkconfig volclava off
    chkconfig --del volclava

    if rpm -qa | grep volclava-2.1* > /dev/null 2>&1; then
       rpm -e volclava-2.1*
    fi
fi

rm -f /etc/init.d/volclava*
rm -f /etc/profile.d/volclava.*
rm -rf $1

echo "volclava has been uninstalled."

#!/bin/bash
function get_os_info(){
    local os_name="unknown"
    local os_version="unknown"
    local cpu_arch="unknown"
    
    #Check OS name and version
    if [ -f /etc/os-release ]; then
        . /etc/os-release
       
        #check ubuntu
        if [ "$ID" = "ubuntu" ] || [ "$ID_LIKE" = "debian" ]; then
            os_name="ubuntu"
            os_version=$VERSION_ID
            
        #check centos
        elif [ "$ID" = "centos" ]; then
            os_name="centos"
            os_version=$VERSION_ID
            
        #check rocky
        elif [ "$ID" = "rocky" ]; then
            os_name="rocky"
            os_version=$VERSION_ID
            
        #check redhat
        elif [ "$ID" = "rhel" ] || [ "$ID_LIKE" = "rhel" ] || [ "$NAME" = "Red Hat Enterprise Linux" ]; then
            os_name="redhat"
            os_version=$VERSION_ID
        fi  
    # Compatible with older systems without /etc/os-release
    else
        if [ -f /etc/redhat-release ]; then
            release=$(cat /etc/redhat-release)
            # Check CentOS
            if echo "$release" | grep -qi "centos"; then
                os_name="centos" 
                os_version=$(echo "$release" | awk '{print $4}' | cut -d '.' -f 1)
            # Check RedHat
            elif echo "$release" | grep -qi "red hat"; then
                os_name="redhat"
                os_version=$(echo "$release" | awk '{print $7}' | cut -d '.' -f 1)
            fi  
        elif [ -f /etc/lsb-release ]; then
            . /etc/lsb-release
            if [ "$DISTRIB_ID" = "Ubuntu" ]; then
                os_name="ubuntu" 
                os_version=$DISTRIB_RELEASE
            fi  
        fi  
    fi  

    # Check main version
    os_version=$(echo "$os_version" | sed -E 's/[^0-9.]//g' | cut -d '.' -f 1)    
    cpu_arch=$(uname -m)
    echo $os_name $os_version $cpu_arch
}

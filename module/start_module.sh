#!/bin/bash

aconnect -x
rmmod cmidid
insmod cmidid.ko requested_gpios=22,4,345,435,1,2
aconnect 24:0 128:0

# MODNAME=$(cat cmidid_main.h | grep "#define MODULE_NAME" | awk '{print $3}' | sed -e 's/"//g')
# VERSION=$(dmesg | grep -i "registered character device" | tail -n 1 | awk '{print $7}' | sed -e 's/\.//')

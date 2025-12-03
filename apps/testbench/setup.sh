#!/bin/bash

iface=$1
pci=$2
if [ "${iface}" == "" ]; then
	iface="enp59s0np0"
fi
if [ "${pci}" == "" ]; then
	pci="3b:01.0"
fi

echo 0 > /sys/class/net/${iface}/device/sriov_numvfs
sleep 1
echo 1 > /sys/class/net/${iface}/device/sriov_numvfs
ip link set ${iface} vf 0 trust on

ifconfig ${iface} 10.10.1.2/24 up

./usertools/dpdk-devbind.py -b vfio-pci ${pci}

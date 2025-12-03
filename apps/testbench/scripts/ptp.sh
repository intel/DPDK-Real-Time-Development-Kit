#!/bin/bash
#
# Copyright (C) 2021 Linutronix GmbH
# Author Kurt Kanzenbach <kurt@linutronix.de>
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Start ptp and synchronize system to network time.
#

set -e

CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Ensure This Script is Run with Root Privileges
if [ $(id -u) -ne 0 ]; then
    echo -e "${RED}This script must be run as root (often via sudo)${NC}"
    exit 1
fi

cd "$(dirname "$0")"

# Interface
INTERFACE=$1
[ -z $INTERFACE ] && INTERFACE="eth0"
echo -e "${CYAN}Running time software on $INTERFACE ${NC}"

# Kill already running daemons
echo -e "${CYAN}Stopping time sync software ${NC}"
pkill ptp4l || true
pkill phc2sys || true

# Stop ntpd
echo -e "${CYAN}Stopping ntp daemons${NC}"
systemctl stop systemd-timesyncd || true
systemctl stop ntpd || true

# Start ptp with 802.1AS-2011 endstation profile
echo -e "${CYAN}Start PTP4L${NC}"
echo -e "${YELLOW}ptp4l -2 -H -i ${INTERFACE} --socket_priority=4 --tx_timestamp_timeout=40 -f ./gPTP.cfg -m &>/var/log/ptp4l.log${NC}"
ptp4l -2 -H -i ${INTERFACE} --socket_priority=4 --tx_timestamp_timeout=40 -f ./gPTP.cfg -m &>/var/log/ptp4l.log &

# Wait for ptp4l
echo -e "${CYAN}Waiting for ptp4l to stabilize${NC}"
sleep 10

# Configure UTC-TAI offset
echo -e "${CYAN}Run PMC${NC}"
pmc -u -b 0 -t 1 "SET GRANDMASTER_SETTINGS_NP clockClass 248 \
        clockAccuracy 0xfe offsetScaledLogVariance 0xffff \
        currentUtcOffset 37 leap61 0 leap59 0 currentUtcOffsetValid 1 \
        ptpTimescale 1 timeTraceable 1 frequencyTraceable 0 \
        timeSource 0xa0" > /dev/null 2>&1

# Synchronize system to network time
echo -e "${CYAN}Start pch2sys${NC}"
echo -e "${YELLOW}phc2sys -s ${INTERFACE} --step_threshold=1 --transportSpecific=1 -w -ml 7 &>/var/log/phc2sys.log${NC}"
phc2sys -s ${INTERFACE} --step_threshold=1 --transportSpecific=1 -w -ml 7 &>/var/log/phc2sys.log &

exit 0

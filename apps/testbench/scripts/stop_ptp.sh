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

# Kill already running daemons
echo -e "${CYAN}Stopping time sync software on $INTERFACE ${NC}"
pkill ptp4l || true
pkill phc2sys || true

exit 0

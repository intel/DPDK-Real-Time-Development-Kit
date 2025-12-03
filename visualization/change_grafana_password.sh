#!/bin/bash
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (C) 2024 Intel Corporation
# Author Walfred Tedeschi <walfred.tedeschi@intel.com>
#

# Check if exactly one argument is provided
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <new_password>"
  exit 1
fi

id=$(docker ps -aqf "name=grafana")
sudo docker exec -ti $id grafana cli admin reset-admin-password ${1}

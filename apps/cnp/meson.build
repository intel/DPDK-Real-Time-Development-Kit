# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025-2025 Intel Corporation

# meson file for building the CNP Tuning Tool

# Source files for the CNP Tuning Tool
sources = files(
  'keyboard.c',
  'cnp-tuning.c',
  'log.c',
  'mqtt.c',
  'parse-args.c',
  'port-setup.c',
  'rxtx.c',
  'stats.c',
)

# Prepare dependencies list
cnp_deps = [dpdk_dep, threads_dep]
if get_option('enable_mqtt')
    cnp_deps += mosquitto_dep
endif

# Include directories
inc_dirs = include_directories('.')

# Build the main CNP executable
cnp_exe = executable('cnp',
    sources,
    dependencies: cnp_deps,
    include_directories: inc_dirs,
    install: true,
    install_dir: get_option('bindir')
)

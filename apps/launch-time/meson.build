# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025-2025 Intel Corporation

# meson file for building the Launch Time testing tool (lttt)

# Source files for the Launch Time testing tool
sources = files(
  'keyboard.c',
  'launch-time.c',
  'log.c',
  'mqtt.c',
  'parse-args.c',
  'port-setup.c',
  'rxtx.c',
  'stats.c',
)

# Prepare dependencies list
lttt_deps = [dpdk_dep, threads_dep]
if get_option('enable_mqtt')
    lttt_deps += mosquitto_dep
endif

# Include directories
inc_dirs = include_directories('.')

# Build the main launch-time executable
lttt_exe = executable('lttt',
    sources,
    dependencies: lttt_deps,
    include_directories: inc_dirs,
    install: true,
    install_dir: get_option('bindir')
)

# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025-2025 Intel Corporation

# meson file for building the RATT application

# Source files for the RATT application
sources = files(
        'ratt.c',
        'stats.c',
        'log.c',
        'mqtt.c',
        'port-setup.c',
        'parse-args.c',
        'keyboard.c',
        'reference.c',
        'mirror.c',
)

# Prepare dependencies list
ratt_deps = [dpdk_dep, threads_dep]
if get_option('enable_mqtt')
    ratt_deps += mosquitto_dep
endif

# Include directories
inc_dirs = include_directories('.')

# Build the main ratt executable
ratt_exe = executable('ratt',
    sources,
    dependencies: ratt_deps,
    include_directories: inc_dirs,
    install: true,
    install_dir: get_option('bindir')
)

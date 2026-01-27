# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation

# meson file, for building this example as part of a main DPDK build.
#
# To build this example as a standalone application with an already-installed
# DPDK instance, use 'make'

sources = files(
    'config.c',
    'dcp_thread.c',
    'lport.c',
    'functions.c',
    'hist.c',
    'layer2_thread.c',
    'lldp_thread.c',
    'log.c',
    'logviamqtt.c',
    'packet_handler.c',
    'tsn.c',
    'rta_thread.c',
    'rtc_thread.c',
    'security.c',
    'stat.c',
    'thread_timer.c',
    'tsn_thread.c',
    'tx_time.c',
    'udp_thread.c',
    'utils.c',
)

# Prepare dependencies list
if not openssl_dep.found()
    openssl_dep = dependency('openssl', required: true)
endif
if not yaml_dep.found()
    yaml_dep = dependency('yaml-0.1', required: true)
endif
tsn_tb_deps = [dpdk_dep, threads_dep, openssl_dep, yaml_dep]
if get_option('enable_mqtt')
    tsn_tb_deps += mosquitto_dep
endif

# Add pflow PMD library explicitly from local DPDK installation
# Link directly to the shared library file
dpdk_lib_dir = meson.project_source_root() + '/external/install/lib/x86_64-linux-gnu'
dpdk_pmd_lib = dpdk_lib_dir + '/librte_net_pflow.so'
dpdk_bus_pci_lib = dpdk_lib_dir + '/librte_bus_pci.so'
dpdk_bus_vdev_lib = dpdk_lib_dir + '/librte_bus_vdev.so'
pflow_dep = declare_dependency(
    link_args: [dpdk_pmd_lib, dpdk_bus_pci_lib, dpdk_bus_vdev_lib]
)
tsn_tb_deps += pflow_dep

# Include directories
inc_dirs = include_directories('.')

# Build the main tsn-testbench executable
tsn_tb_exe = executable('tsn_tb',
    sources,
    dependencies: tsn_tb_deps,
    include_directories: inc_dirs,
    install: true,
    install_dir: get_option('bindir')
)

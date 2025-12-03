# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation

project('lttt', 'c',
    version: run_command(find_program('cat', 'more'),
        files('VERSION'), check: true).stdout().strip(),
    license: 'BSD-3-Clause',
    default_options: [
        'buildtype=release',
        'default_library=shared',
        'warning_level=2',
        'werror=false',
        'c_std=gnu99',
    ],
    meson_version: '>= 1.5.0'
)

lttt_conf = configuration_data()

##################################################################
# set the major version, which might be used by pmds and libraries
# depending on the configuration options
pver = meson.project_version().split('.')
major_version = '@0@.@1@'.format(pver.get(0), pver.get(1))

# extract all version information into the build configuration
lttt_conf.set('LTIME_VER_YEAR', pver.get(0).to_int())
lttt_conf.set('LTIME_VER_MONTH', pver.get(1).to_int())
if pver.get(2).contains('-rc')
    rc_ver = pver.get(2).split('-rc')
    lttt_conf.set('LTIME_VER_MINOR', rc_ver.get(0).to_int())
    lttt_conf.set_quoted('LTIME_VER_SUFFIX', '-rc')
    lttt_conf.set('LTIME_VER_RELEASE', rc_ver.get(1).to_int())
else
    lttt_conf.set('LTIME_VER_MINOR', pver.get(2).to_int())
    lttt_conf.set_quoted('LTIME_VER_SUFFIX', '')
# for actual, non-rc releases, set the release value to 99 to ensure releases
# have higher version numbers than their respective release candidates
    lttt_conf.set('LTIME_VER_RELEASE', 99)
endif

# Add project description
project_description = 'Launch-Time (Launch-Time Testing Tool) - A tool for testing real-time applications using DPDK'

fs = import('fs')

# DPDK dependency - always use installed version from external/install
# The PKG_CONFIG_PATH should be set by the Makefile to point to external/install/lib/pkgconfig
message('Using installed DPDK from external/install directory')
dpdk_dep = dependency('libdpdk', required: true)

threads_dep = dependency('threads', required: true)

# Required dependencies
openssl_dep = dependency('openssl', required: true)
yaml_dep = dependency('yaml-0.1', required: true)

# Optional dependencies
mosquitto_dep = dependency('libmosquitto', required: get_option('enable_mqtt'))

# Common compile arguments
add_project_arguments('-DALLOW_EXPERIMENTAL_API', language: 'c')

# Conditional compile arguments based on options
if get_option('enable_debug')
    add_project_arguments('-DDEBUG', '-g', language: 'c')
endif

if get_option('max_lcores') != 128
    add_project_arguments('-DMAX_LCORES=' + get_option('max_lcores').to_string(), language: 'c')
endif

if not get_option('enable_mqtt')
    add_project_arguments('-DDISABLE_MQTT', language: 'c')
endif

# Build the main executable
subdir('apps')

# Print configuration summary
summary({
    'prefix': get_option('prefix'),
    'libdir': get_option('libdir'),
    'datadir': get_option('datadir'),
    'bindir': get_option('bindir'),
}, section: 'Directories')

summary({
    'buildtype': get_option('buildtype'),
    'debug': get_option('debug'),
    'optimization': get_option('optimization'),
    'warning_level': get_option('warning_level'),
}, section: 'Build')

summary({
    'DPDK': dpdk_dep.found(),
    'Mosquitto': mosquitto_dep.found(),
    'Threads': threads_dep.found(),
}, section: 'Dependencies')

# Building LTTT with Meson

This document describes how to build the LTTT (Real-time Application Testing Tool) project using Meson.

## Prerequisites

- Meson (>= 1.5.0)
- Ninja build system
- Git (for cloning DPDK)
- Mosquitto development libraries (optional, can be disabled)
- GCC or Clang compiler

**Note:** You do NOT need DPDK pre-installed. The build system automatically clones, patches, builds, and installs DPDK from source into the `external/install` directory.

## Quick Start

```bash
# Build everything (DPDK + LTTT) - recommended
make

# This automatically:
# 1. Clones DPDK from the official repository
# 2. Applies required patches
# 3. Builds DPDK
# 4. Installs DPDK to external/install
# 5. Builds LTTT using the locally installed DPDK
```

## Build Options

You can configure the build using various options:

```bash
# Disable MQTT support
make ENABLE_MQTT=false

# Enable debug build
make BUILD_TYPE=debug ENABLE_DEBUG=true

# Use specific DPDK version
make distclean
make DPDK_GIT_TAG=v24.11

# Custom installation prefix (for Meson)
make
cd builddir
meson configure --prefix=/usr/local
ninja install
```

### Using the Makefile (Recommended)

The Makefile is the recommended way to build LTTT as it handles all dependencies automatically:

```bash
# Build everything
make

# Clean rebuild
make rebuild

# Remove everything including DPDK
make distclean

# Build only DPDK
make dpdk

# See all available options
make help
```

For comprehensive Makefile documentation, see [doc/MAKEFILE.md](doc/MAKEFILE.md).

### Using Meson Directly

If you prefer to use Meson directly, you must first ensure DPDK is installed in the `external/install` directory:

```bash
# First, build and install DPDK
make dpdk

# Then build LTTT with Meson
export PKG_CONFIG_PATH=$PWD/external/install/lib/pkgconfig:$PKG_CONFIG_PATH
meson setup builddir
ninja -C builddir
```

## Build Types

```bash
# Debug build
make BUILD_TYPE=debug

# Release build (default)
make BUILD_TYPE=release

# Release build with debug info
make BUILD_TYPE=debugoptimized
```

## DPDK Version Management

The build system clones DPDK from the official repository. You can control which version is used:

```bash
# Use specific DPDK version
make distclean
make DPDK_GIT_TAG=v24.11

# Use a specific commit
make distclean
make DPDK_GIT_TAG=abc123def

# Use main/master branch
make distclean
make DPDK_GIT_TAG=main
```

## Cross Compilation

```bash
# For cross compilation, specify a cross file
make
cd builddir
meson configure --cross-file=cross_file.txt
ninja
```

## Running

After building, the `lttt` executable will be available in `builddir/apps/launch-time/`:

```bash
# Run from build directory
sudo ./builddir/apps/launch-time/lttt --help

# Or after installation
sudo lttt --help
```

**Note:** DPDK applications require root privileges to access hardware resources.

## Troubleshooting

### DPDK Build Issues

- **Git clone fails**: Check your internet connection and access to github.com
- **Patch application fails**: The patches may not be compatible with the DPDK version you're using
- **DPDK build fails**: Check build logs in `external/dpdk/builddir/meson-logs/`

### LTTT Build Issues

- **DPDK not found**: Run `make dpdk` to ensure DPDK is installed in `external/install`
- **mosquitto not found**: Install libmosquitto-dev or disable MQTT with `make ENABLE_MQTT=false`
- **PKG_CONFIG_PATH issues**: The Makefile sets this automatically; if using Meson directly, ensure it points to `external/install/lib/pkgconfig`

### Debugging Build Issues

To see detailed build configuration:

```bash
# Show all build options
meson configure builddir

# Rebuild with verbose output
ninja -C builddir -v

# Check DPDK installation
ls -la external/install/lib/pkgconfig/libdpdk.pc
pkg-config --modversion libdpdk
```

### Starting Fresh

If you encounter persistent issues:

```bash
# Remove all build artifacts including DPDK
make distclean

# Rebuild everything from scratch
make
```

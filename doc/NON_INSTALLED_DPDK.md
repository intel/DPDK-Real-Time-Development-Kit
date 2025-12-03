# DPDK Build and Installation Process

This document explains how LTTT handles DPDK dependencies.

## Overview

The LTTT build system has been designed to **always** build and use DPDK from source. This ensures:

1. **Version Consistency**: You always use the correct DPDK version
2. **Patch Application**: All required patches are automatically applied
3. **No System Conflicts**: Avoids conflicts with system-installed DPDK versions
4. **Reproducible Builds**: Every developer uses the same DPDK configuration

## How It Works

The build system automatically:

1. **Clones** DPDK from the official repository (configurable version)
2. **Applies** necessary patches from the `patches/` directory
3. **Builds** DPDK using Meson/Ninja
4. **Installs** DPDK into `external/install/` directory
5. **Configures** pkg-config to find the locally installed DPDK
6. **Builds** LTTT using the locally installed DPDK

## Build Process

### Simple Build

```bash
# Build everything (DPDK + LTTT)
make

# This will:
# - Clone DPDK to external/dpdk
# - Apply patches from patches/ directory
# - Build DPDK
# - Install DPDK to external/install
# - Build LTTT using the locally installed DPDK
```

### First Build vs. Subsequent Builds

**First build:**
- Takes longer as it clones and builds DPDK (~5-10 minutes depending on system)
- Creates `external/dpdk/` directory
- Creates `external/install/` directory

**Subsequent builds:**
- Much faster as DPDK is already built
- Only rebuilds LTTT if source files changed
- DPDK is only rebuilt if you explicitly clean it

### Clean Builds

```bash
# Clean LTTT build only (keeps DPDK)
make clean

# Clean everything including DPDK
make distclean
```

## Configuration Methods

### Method 1: Using the Makefile (Recommended)

The Makefile handles everything automatically:

```bash
# Build with default DPDK version (v25.11)
make

# Use specific DPDK version
make distclean
make DPDK_GIT_TAG=v24.11

# Debug build
make BUILD_TYPE=debug ENABLE_DEBUG=true

# See all options
make help
```

### Method 2: Manual Meson Build

If you need to use Meson directly:

```bash
# First, ensure DPDK is installed
make dpdk

# Then build with Meson
export PKG_CONFIG_PATH=$PWD/external/install/lib/pkgconfig:$PKG_CONFIG_PATH
meson setup builddir
ninja -C builddir
```

## Directory Structure

After building, you'll have the following structure:

```
external/
├── dpdk/                    # DPDK source code
│   ├── builddir/           # DPDK build artifacts
│   ├── lib/                # DPDK source libraries
│   └── ...                 # Other DPDK files
└── install/                # DPDK installation directory
    ├── include/            # DPDK header files
    │   └── rte_*.h
    ├── lib/                # DPDK libraries
    │   ├── librte_*.a
    │   └── pkgconfig/
    │       └── libdpdk.pc  # pkg-config file
    └── ...
```

## Build System Changes

### 1. Makefile Changes

The Makefile now:
- Always builds DPDK from source
- Installs DPDK to `external/install/`
- Sets `PKG_CONFIG_PATH` to point to the local installation
- Provides targets for DPDK management: `dpdk`, `dpdk-clone`, `dpdk-patch`, `dpdk-build`, `dpdk-install`, `dpdk-clean`, `dpdk-distclean`

### 2. Meson Build System Changes

The `meson.build` file now:
- Only uses pkg-config to find DPDK
- Expects DPDK to be in `external/install/`
- Relies on `PKG_CONFIG_PATH` being set correctly (handled by Makefile)

### 3. Removed Options

The following Meson options have been removed as they're no longer needed:
- `dpdk_source_dir`
- `dpdk_build_dir`

## DPDK Version Management

### Changing DPDK Version

```bash
# Clean everything
make distclean

# Build with specific version
make DPDK_GIT_TAG=v24.11

# Or set permanently in Makefile by editing DPDK_GIT_TAG variable
```

### Applying Custom Patches

Place your patch files in the `patches/` directory. They will be automatically applied during the build process:

```bash
# Add your patch
cp my-dpdk-fix.patch patches/

# Clean and rebuild DPDK
make dpdk-clean
make dpdk
```

## Troubleshooting

### DPDK Clone Issues

**Problem:** Git clone fails
**Solution:** Check internet connectivity and access to github.com

### Patch Application Issues

**Problem:** Patches fail to apply
**Solution:**
- Check that patches are compatible with the DPDK version
- Review patch files in `patches/` directory
- Try without patches by temporarily renaming the patches directory

### Build Failures

**Problem:** DPDK build fails
**Solution:**
- Check build logs in `external/dpdk/builddir/meson-logs/`
- Ensure you have all dependencies: `meson`, `ninja`, `python3`, build tools
- Try a clean rebuild: `make dpdk-clean && make dpdk`

### PKG_CONFIG Issues

**Problem:** LTTT cannot find DPDK
**Solution:**
- Verify DPDK is installed: `ls external/install/lib/pkgconfig/libdpdk.pc`
- When using Meson directly, ensure PKG_CONFIG_PATH is set:
  ```bash
  export PKG_CONFIG_PATH=$PWD/external/install/lib/pkgconfig:$PKG_CONFIG_PATH
  ```
- Check pkg-config can find it: `pkg-config --modversion libdpdk`

## Advanced Usage

### Using a Different DPDK Repository

```bash
# Edit Makefile or override on command line
make DPDK_GIT_URL=https://github.com/my-org/my-dpdk-fork.git DPDK_GIT_TAG=my-branch
```

### Manual DPDK Build Steps

If you need to build DPDK manually:

```bash
# Clone
make dpdk-clone

# Apply patches
make dpdk-patch

# Build
make dpdk-build

# Install
make dpdk-install

# Or all at once
make dpdk
```

### Debugging the Build

```bash
# Verbose build output
ninja -C builddir -v

# Check Meson configuration
meson configure builddir

# Check DPDK version
pkg-config --modversion libdpdk
```

## Benefits of This Approach

### Consistency
- Every developer uses the exact same DPDK version
- No "it works on my machine" issues due to different system DPDK versions
- Reproducible builds across different environments

### Patch Management
- All required patches are automatically applied
- No manual patching steps required
- Patches are version-controlled in the repository

### Simplicity
- Single command to build everything: `make`
- No need to manually install DPDK system-wide
- No need to manage system package dependencies for DPDK

### Isolation
- Doesn't interfere with system-installed DPDK
- Multiple projects can use different DPDK versions
- Easy to test with different DPDK versions

## Migration from Previous Build System

If you were previously using `USE_DPDK_SOURCE=true`:
- Remove the `USE_DPDK_SOURCE=true` flag - it's now the default behavior
- Simply run `make` to build

If you were using system-installed DPDK:
- The build system now always builds DPDK locally
- Your first build will take longer to clone and build DPDK
- Subsequent builds will be just as fast as before

## Summary

The LTTT build system now follows a "batteries included" approach:
- DPDK is automatically cloned, patched, built, and installed locally
- No system-wide DPDK installation required
- Ensures version consistency and proper patch application
- Simplifies the build process to a single `make` command


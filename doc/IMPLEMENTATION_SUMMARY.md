# Non-Installed DPDK Support - Implementation Summary

## Overview

The LTTT Meson build system has been successfully updated to support building with non-installed DPDK builds. This allows developers to use locally-built DPDK versions instead of requiring system-wide DPDK installation.

## Changes Made

### 1. Meson Configuration Files

#### `meson_options.txt` - New Build Options

Added two configuration options:

```console
dpdk_build_dir     : Path to DPDK build directory (empty by default)
dpdk_source_dir    : Path to DPDK source directory (empty by default)
```

#### `meson.build` - Enhanced DPDK Detection

Implemented intelligent dependency resolution with the following logic:

1. **Check Meson options first** (`-Ddpdk_build_dir` and `-Ddpdk_source_dir`)
2. **Check environment variables** (`RTE_SDK_BUILD` and `RTE_SDK`)
3. **Use non-installed DPDK** if paths provided and valid
4. **Fall back to pkg-config** for installed DPDK if no paths specified

Key implementation features:

- **Path validation**: Checks that directories exist and contain expected files
- **Flexible linking**: Supports both static (`libdpdk.a`) and shared (`libdpdk.so`) builds
- **Clear messaging**: Informs users which DPDK configuration is being used
- **Error handling**: Provides specific error messages for configuration issues

### 2. Makefile Build System

#### `Makefile` - Unified Build Interface

Created a comprehensive Makefile that provides an easy interface for building the project:

```bash
make [target] [VARIABLE=value]
```

**Supported targets:**

- `make` or `make build`: Build the project
- `make rebuild`: Clean and rebuild
- `make USE_DPDK_SOURCE=true`: Build with DPDK from source (automatic)
- `make dpdk-source`: Clone, patch, and build DPDK
- `make help`: Show all available options
- `-t, --build-type TYPE`: Choose build type (release/debug/debugoptimized)
- `--enable-debug`: Add debug logging
- `--disable-mqtt`: Disable MQTT support
- `-b, --build-dir DIR`: Specify output build directory
- `--clean`: Clean and reconfigure
- `--no-build`: Setup only, don't compile

### 3. Documentation

#### `NON_INSTALLED_DPDK.md` - Comprehensive Guide

Complete documentation covering:

- Configuration methods (4 different approaches)
- Example scenarios and workflows
- Technical implementation details
- Validation and error handling
- Troubleshooting guide
- Future enhancement possibilities

#### `MESON_BUILD.md` - Updated Build Instructions

Added new section covering:

- Non-installed DPDK usage
- Environment variable configuration
- Example commands
- Make vs. Meson-built DPDK usage

#### `DPDK_BUILD_CHANGES.md` - Change Summary

Summary of all changes, compatibility notes, and migration guide.

## Usage Examples

### Example 1: Build with Local DPDK

```bash
# First, build DPDK locally
cd /tmp/dpdk
git clone https://github.com/DPDK/dpdk.git
cd dpdk
meson setup build
meson compile -C build

# Then build LTTT with that DPDK
cd /work/projects/intel/io-engines/rkwiles.launch-time
meson setup builddir -Ddpdk_build_dir=/tmp/dpdk/build
ninja -C builddir
```

### Example 2: Using Makefile

```bash
# Build with system DPDK
make

# Or build with DPDK from source (automatic)
make USE_DPDK_SOURCE=true
```

### Example 3: Environment Variables

```bash
export RTE_SDK_BUILD=/path/to/dpdk/build
export RTE_SDK=/path/to/dpdk/source
meson setup builddir
meson compile -C builddir
```

### Example 4: Multiple Build Configurations

```bash
# Release with system DPDK
make BUILD_DIR=builddir-rel BUILD_TYPE=release

# Debug with DPDK from source
make BUILD_DIR=builddir-dbg BUILD_TYPE=debug ENABLE_DEBUG=true USE_DPDK_SOURCE=true

# Or compile separately
meson compile -C builddir-rel
meson compile -C builddir-dbg
```

## Technical Implementation

### Directory Structure Expected

The implementation assumes this layout for a non-installed DPDK build:

```consolec
<dpdk_build_dir>/
├── include/
│   ├── rte_config.h      (required)
│   └── rte_*.h           (DPDK headers)
└── lib/
    └── libdpdk.a or libdpdk.so   (required)
```

### Meson Dependency Declaration

For non-installed DPDK, a custom dependency is created:

```meson
dpdk_dep = declare_dependency(
    include_directories: [
        include_directories(build_dir / 'include'),
        include_directories(source_dir / 'lib')  # optional
    ],
    link_args: ['-L' + build_dir / 'lib', '-ldpdk']
)
```

### Priority Resolution Order

1. `-Ddpdk_build_dir` Meson option → Use specified build directory
2. `RTE_SDK_BUILD` environment variable → Use build directory from environment
3. `-Ddpdk_source_dir` Meson option → Use source headers only
4. `RTE_SDK` environment variable → Use source headers from environment
5. Default (installed DPDK) → Use pkg-config to find system DPDK

## Key Features

✓ **Backward Compatible** - Existing installed DPDK builds work unchanged

✓ **Environment Variable Support** - Respects `RTE_SDK_BUILD` and `RTE_SDK`

✓ **Flexible** - Works with DPDK built by Meson or Make

✓ **Validation** - Checks paths exist and contain expected files

✓ **Clear Messaging** - Tells users which DPDK is being used

✓ **Error Handling** - Specific, actionable error messages

✓ **Cross-Platform** - Design supports Linux and other Unix systems

## Files Modified

| File | Changes |
|------|---------|
| `meson_options.txt` | Added `dpdk_build_dir` and `dpdk_source_dir` options |
| `meson.build` | Implemented DPDK detection logic with environment variable support |
| `Makefile` | New unified build system |
| `NON_INSTALLED_DPDK.md` | New comprehensive documentation |
| `MESON_BUILD.md` | Updated with non-installed DPDK section |
| `DPDK_BUILD_CHANGES.md` | New summary of changes |

## Backward Compatibility

✓ Existing builds continue to work without modification

✓ Default behavior (installed DPDK via pkg-config) unchanged

✓ No breaking changes to build system

✓ All new options are optional (empty strings by default)

## Validation Status

- ✓ Builds successfully with installed DPDK (default)
- ✓ Meson setup completes without errors
- ✓ New build options appear in `meson configure` output
- ✓ Helper script functions correctly
- ✓ Environment variable detection working
- ✓ Error messages clear and helpful

## Migration Guide

### For Current Users (Installed DPDK)

**No changes needed.** Continue using:

```bash
meson setup builddir
ninja -C builddir
```

### For New Users (Non-Installed DPDK)

**Choose one method:**

1. **Easiest** - Use helper script:

   ```bash
   make
   ```

2. **Portable** - Use environment variables:

   ```bash
   export RTE_SDK_BUILD=/path/to/dpdk/build
   meson setup builddir
   ninja -C builddir
   ```

3. **Direct** - Use Meson options:

   ```bash
   meson setup builddir -Ddpdk_build_dir=/path/to/dpdk/build
   ninja -C builddir
   ```

4. **Integrated** - Use in scripts:

   ```bash
   make setup
   ninja -C builddir
   ```

## Troubleshooting

### Issue: "DPDK build directory does not exist"

**Solution:** Verify the path is absolute and contains a valid DPDK build with `include/` and `lib/` subdirectories.

### Issue: "libdpdk not found at link time"

**Solution:** Ensure `lib/libdpdk.a` or `lib/libdpdk.so` exists in the specified DPDK build directory.

### Issue: "rte_config.h not found"

**Solution:** Verify that `include/rte_config.h` exists in the DPDK build directory. For Make-built DPDK, this file may be generated during the build process.

### Debug: Check actual configuration

```bash
meson configure builddir
```

This shows all current build options and their values.

## Next Steps

To use this new functionality:

1. **Read** `NON_INSTALLED_DPDK.md` for detailed instructions
2. **Try** one of the usage examples above
3. **Use** `make help` for all available options
4. **Reference** `DPDK_BUILD_CHANGES.md` for technical details

## Support & Questions

For detailed information, see:

- `MESON_BUILD.md` - General build instructions
- `NON_INSTALLED_DPDK.md` - Non-installed DPDK guide
- `DPDK_BUILD_CHANGES.md` - Implementation details
- `make help` - Build system usage help

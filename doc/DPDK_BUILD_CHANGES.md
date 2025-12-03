# Meson Build System Changes for Non-Installed DPDK Support

## Summary

The LTTT Meson build system has been enhanced to support using non-installed DPDK builds. This enables developers to build LTTT with a locally-built DPDK version, useful for development, testing with specific DPDK versions, or when DPDK is not available through system package managers.

## What Changed

### 1. New Meson Build Options

Added two new options to `meson_options.txt`:

```console
-Ddpdk_build_dir: Path to DPDK build directory (for non-installed DPDK)
-Ddpdk_source_dir: Path to DPDK source directory (for better header resolution)
```

### 2. Enhanced Dependency Detection

Updated `meson.build` to implement a smart dependency resolution strategy:

**Priority order:**

1. Check `-Ddpdk_build_dir=/path/to/builddir` option
2. Check `RTE_SDK_BUILD` environment variable
3. Check `-Ddpdk_source_dir` option
4. Check `RTE_SDK` environment variable
5. Fall back to installed DPDK via pkg-config

**Key features:**

- Validates paths exist before use
- Provides clear error messages for invalid configurations
- Creates custom `declare_dependency()` objects for non-installed DPDK
- Maintains backward compatibility with installed DPDK

### 3. Documentation

Created comprehensive documentation in `NON_INSTALLED_DPDK.md`:

- Configuration methods and examples
- Scenario-based guides
- Troubleshooting section
- Technical implementation details

Updated `MESON_BUILD.md` with non-installed DPDK instructions.

## How to Use

### Method 1: Using Makefile (Recommended)

```bash
# Build with system-installed DPDK
make

# Or build with DPDK from source
make USE_DPDK_SOURCE=true
```

### Method 2: Direct Meson/Ninja

```bash
meson setup builddir -Ddpdk_build_dir=/path/to/dpdk/build
ninja -C builddir
```

### Method 3: Environment Variables

```bash
export RTE_SDK_BUILD=/path/to/dpdk/build
meson setup builddir
ninja -C builddir
```

### Method 4: Default (Installed DPDK)

```bash
meson setup builddir
ninja -C builddir
```

## Implementation Details

### Dependency Declaration

For non-installed DPDK, the system creates:

```meson
dpdk_dep = declare_dependency(
    include_directories: [
        include_directories(build_dir / 'include'),
        include_directories(source_dir / 'lib')  # if provided
    ],
    link_args: ['-L' + build_dir / 'lib', '-ldpdk']
)
```

### Directory Structure Assumptions

Expected DPDK build directory layout:

```console
dpdk_build_dir/
├── include/
│   ├── rte_config.h
│   ├── rte_*.h
│   └── ...
└── lib/
    ├── libdpdk.a (or libdpdk.so)
    └── ...
```

## Compatibility

- **Backward compatible**: Existing builds with installed DPDK work unchanged
- **Supports both Meson and Make builds**: Can use DPDK built with either system
- **Environment variable support**: Works with existing DPDK shell environment setups
- **Cross-platform**: Design supports both Linux and other Unix-like systems

## Example Workflows

### Quick Local Development

```bash
# Build DPDK
cd /tmp/dpdk
meson setup build
ninja -C build

# Set environment and build LTTT
export RTE_SDK_BUILD=/tmp/dpdk/build
cd /work/projects/intel/io-engines/rkwiles.launch-time
meson setup builddir
ninja -C builddir
```

## Example Workflows

### Multiple Build Configurations

```bash
# Release build
make BUILD_DIR=builddir-release BUILD_TYPE=release

# Debug build
make BUILD_DIR=builddir-debug BUILD_TYPE=debug ENABLE_DEBUG=true

# Without MQTT
make BUILD_DIR=builddir-no-mqtt ENABLE_MQTT=false
```

### CI/CD Integration

```bash
# Build with system DPDK in CI
make setup  # Setup only, for validation
ninja -C builddir
```

## Files Modified/Created

- **Makefile** - New unified build system
- **meson.build** - Enhanced DPDK dependency detection
- **meson_options.txt** - Added new build options
- **doc/MAKEFILE.md** - Comprehensive Makefile documentation
- **doc/NON_INSTALLED_DPDK.md** - DPDK configuration documentation
- **doc/MESON_BUILD.md** - Updated with non-installed DPDK section
- **README.md** - Reference to new build capabilities

## Testing

The changes have been verified to:

- ✓ Work with installed DPDK via pkg-config (default)
- ✓ Detect and use environment variables
- ✓ Properly handle new Meson options
- ✓ Provide meaningful error messages for invalid paths
- ✓ Maintain backward compatibility
- ✓ Build successfully with the current setup

## Migration Path

**For users currently using installed DPDK:**

- No action required - continue as before
- All existing commands work unchanged

**For users wanting to use non-installed DPDK:**

- Choose one of the methods above
- Start with the Makefile (`make USE_DPDK_SOURCE=true`) for easiest experience
- Or set `RTE_SDK_BUILD` environment variable
- Advanced users can use Meson options directly

## Error Handling

The system provides clear error messages for common issues:

```console
ERROR: DPDK build directory does not exist: /invalid/path
ERROR: DPDK lib directory not found in: /path/lib
```

Run `meson setup builddir -Ddpdk_build_dir=...` with verbose flag for more details:

```bash
meson setup builddir -Ddpdk_build_dir=/path -v
```

## Future Considerations

Potential enhancements:

- Support for DPDK as a Meson subproject
- Automatic DPDK version validation
- Support for multiple DPDK versions
- Enhanced cross-compilation support
- Integration with container-based builds

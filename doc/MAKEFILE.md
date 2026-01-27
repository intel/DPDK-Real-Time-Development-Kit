<!-- SPDX-License-Identifier: BSD-3-Clause -->
<!-- Copyright(c) 2025 Intel Corporation -->

# Makefile Build System

The Makefile provides a simplified build interface for the Launch-Time Testing Tool (lttt) using a standard make-based build system.

## Quick Start

### Build with System DPDK
```bash
make
```

### Build with DPDK from Source
```bash
make USE_DPDK_SOURCE=true
```

### Clean and Rebuild
```bash
make rebuild
```

## Available Targets

### Main Targets
- **`make`** or **`make all`** - Build the project (default target)
- **`make build`** - Build the project
- **`make setup`** - Setup meson build without building
- **`make rebuild`** - Clean and rebuild the project
- **`make clean`** - Clean build artifacts
- **`make distclean`** - Clean everything including DPDK
- **`make install`** - Install the built executable
- **`make run`** - Run the executable (requires sudo)
- **`make check`** - Verify executable exists and show info
- **`make help`** - Show help message with all available targets

### DPDK Targets
- **`make dpdk-source`** - Clone, patch, and build DPDK from source
- **`make dpdk-patch`** - Apply patches to DPDK source
- **`make dpdk-build`** - Build DPDK (assumes source already cloned)
- **`make dpdk-clean`** - Clean DPDK build
- **`make dpdk-distclean`** - Remove DPDK completely

### Utility Targets
- **`make config`** - Display current build configuration
- **`make show-config`** - Display configuration and meson options

### Utility Targets
- **`make config`** - Display current build configuration
- **`make show-config`** - Display configuration and meson options
- **`make check`** - Check if executable exists

## Configuration Variables

You can customize the build by setting these variables on the command line:

| Variable | Default | Description |
|----------|---------|-------------|
| `BUILD_DIR` | `builddir` | Build directory path |
| `BUILD_TYPE` | `release` | Build type: `release`, `debug`, `debugoptimized` |
| `ENABLE_MQTT` | `true` | Enable MQTT support |
| `ENABLE_DEBUG` | `false` | Enable debug logging |
| `USE_DPDK_SOURCE` | `false` | Use DPDK from source instead of system |
| `DPDK_GIT_URL` | `https://github.com/DPDK/dpdk.git` | DPDK repository URL |
| `DPDK_GIT_TAG` | `v25.11` | DPDK version tag/branch |

## Examples

### 1. Standard Build with System DPDK
```bash
make
```
This uses the DPDK installed on your system via package manager.

### 2. Build with Local DPDK from Source
```bash
make USE_DPDK_SOURCE=true
```
This clones DPDK, applies patches, builds it, then builds lttt against it.

### 3. Debug Build
```bash
make BUILD_TYPE=debug ENABLE_DEBUG=true
```

### 4. Build without MQTT Support
```bash
make ENABLE_MQTT=false
```

### 5. Custom DPDK Version
```bash
make USE_DPDK_SOURCE=true DPDK_GIT_TAG=v24.11
```

### 6. Clean Rebuild
```bash
make rebuild
```

### 7. Build and Run
```bash
make && sudo ./builddir/src/lttt --help
```
or simply:
```bash
make run
```

### 8. Complete Clean (including DPDK)
```bash
make distclean
```

## Workflow Examples

### First Time Setup with System DPDK
```bash
# Install DPDK first (example for Ubuntu/Debian)
sudo apt-get install dpdk dpdk-dev

# Build the project
make
```

### First Time Setup with DPDK from Source
```bash
# Single command - clones, patches, builds DPDK, then builds lttt
make USE_DPDK_SOURCE=true

# Or step by step
make dpdk-source              # Clone and build DPDK
make build USE_DPDK_SOURCE=true  # Build lttt against it
```

### Development Workflow
```bash
# Edit source files...

# Quick rebuild
make

# Clean rebuild if needed
make rebuild

# Run tests
make run
```

### Switching Between DPDK Versions
```bash
# Clean everything
make distclean

# Build with different DPDK version
make USE_DPDK_SOURCE=true DPDK_GIT_TAG=v24.11
```

## Advantages

1. **Standard Interface** - Uses familiar `make` commands
2. **Incremental Builds** - Only rebuilds what changed
3. **Dependency Tracking** - Automatically handles DPDK dependencies
4. **Parallel Execution** - Can use `make -j` for parallel builds
5. **Better IDE Integration** - Most IDEs understand Makefiles
6. **Simpler Commands** - `make` with clear configuration variables
7. **Tab Completion** - Shell completion works with make targets

## Troubleshooting

### Build Fails
```bash
# Try a clean rebuild
make rebuild

# Or complete clean
make distclean && make
```

### DPDK Issues
```bash
# Clean and rebuild DPDK
make dpdk-clean
make dpdk-source
make build USE_DPDK_SOURCE=true
```

### Check Configuration
```bash
make show-config
```

## Notes

- The Makefile uses Meson and Ninja as the actual build system
- All Meson options from `meson_options.txt` are supported
- Color output helps distinguish different types of messages

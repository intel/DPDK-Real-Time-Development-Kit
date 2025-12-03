# Makefile Quick Reference

## Most Common Commands

```bash
# Build with system DPDK (most common)
make

# Build with DPDK from source
make USE_DPDK_SOURCE=true

# Clean and rebuild
make rebuild

# Get help
make help

# Check if built successfully
make check
```

## All Available Targets

### Building

- `make` or `make all` - Build the project
- `make build` - Build the project
- `make rebuild` - Clean and rebuild
- `make setup` - Setup meson build (without building)

### Cleaning

- `make clean` - Remove build artifacts
- `make distclean` - Remove everything including DPDK

### DPDK

- `make dpdk-source` - Clone, patch, and build DPDK
- `make dpdk-build` - Build DPDK only
- `make dpdk-patch` - Apply patches to DPDK
- `make dpdk-clean` - Clean DPDK build
- `make dpdk-distclean` - Remove DPDK completely

### Utility

- `make install` - Install the executable
- `make run` - Run the executable (requires sudo)
- `make check` - Check if executable exists
- `make config` - Show configuration
- `make show-config` - Show configuration and meson options
- `make help` - Show complete help

## Configuration Variables

Set these on the command line: `make VARIABLE=value`

- `BUILD_DIR=path` - Build directory (default: builddir)
- `BUILD_TYPE=type` - Build type: release, debug, debugoptimized
- `ENABLE_MQTT=bool` - Enable MQTT (default: true)
- `ENABLE_DEBUG=bool` - Enable debug logging (default: false)
- `USE_DPDK_SOURCE=bool` - Use DPDK from source (default: false)
- `DPDK_GIT_URL=url` - DPDK repository URL
- `DPDK_GIT_TAG=tag` - DPDK version (default: v25.11)

## Quick Examples

```bash
# Standard build
make

# Debug build with logging
make BUILD_TYPE=debug ENABLE_DEBUG=true

# Build with DPDK v24.11 from source
make USE_DPDK_SOURCE=true DPDK_GIT_TAG=v24.11

# Build without MQTT
make ENABLE_MQTT=false

# Complete clean and rebuild
make distclean && make

# Build and install
make && make install

# Build DPDK separately, then build project
make dpdk-source
make build USE_DPDK_SOURCE=true
```

## Troubleshooting

```bash
# Build fails? Try clean rebuild
make rebuild

# Still failing? Complete clean
make distclean && make

# Check configuration
make show-config

# DPDK issues? Clean and rebuild DPDK
make dpdk-clean dpdk-source
make build USE_DPDK_SOURCE=true
```

## Color Legend

- 🔵 **Blue** - Section headers and informational messages
- 🟢 **Green** - Success messages and completion
- 🟡 **Yellow** - Warnings and existing state notices
- 🔴 **Red** - Errors (if any occur)

## Additional Help

- Full documentation: `doc/MAKEFILE.md`
- Meson details: `MESON_BUILD.md`
- Migration guide: `doc/BUILD_SYSTEM_MIGRATION.md`
- Online help: `make help`

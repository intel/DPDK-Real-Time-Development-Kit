# Quick Reference: Building LTTT with DPDK

## TL;DR - Quick Start

```bash
# Option 1: Build with system DPDK
cd /work/projects/intel/io-engines/rkwiles.launch-time
make

# Option 2: Build with DPDK from source (clones, patches, and builds automatically)
cd /work/projects/intel/io-engines/rkwiles.launch-time
make USE_DPDK_SOURCE=true
```

## Three Ways to Configure

### Way 1: Makefile (Easiest & Recommended)

```bash
# Build with system-installed DPDK
make

# Or build with DPDK from source (in external/dpdk)
make USE_DPDK_SOURCE=true

# Debug build
make ENABLE_DEBUG=true BUILD_TYPE=debug

# See all options
make help
```

### Way 2: Environment Variables (For Pre-Built DPDK)

```bash
export RTE_SDK_BUILD=/path/to/dpdk/build    # DPDK build
export RTE_SDK=/path/to/dpdk/source         # Optional
meson setup builddir
ninja -C builddir
```

### Way 3: Meson Options (For Pre-Built DPDK)

```bash
meson setup builddir -Ddpdk_build_dir=/path/to/dpdk/build
meson setup builddir -Ddpdk_build_dir=/path/to/dpdk/build -Ddpdk_source_dir=/path/to/source
ninja -C builddir
```

### Way 4: Installed DPDK (Unchanged Default)

```bash
meson setup builddir  # Uses pkg-config to find DPDK
ninja -C builddir
```

## Common Scenarios

### Scenario 1: CI/CD Pipeline

```bash
#!/bin/bash
export RTE_SDK_BUILD=/ci/builds/dpdk-25.11
cd /project/rkwiles.launch-time
make setup   # Setup only
ninja -C builddir
meson test -C builddir        # Run tests if available
```

### Scenario 2: Multiple Builds (Release vs Debug)

```bash
# Release build
make BUILD_DIR=builddir-rel BUILD_TYPE=release

# Debug build
make BUILD_DIR=builddir-dbg BUILD_TYPE=debug ENABLE_DEBUG=true

# Compile both
ninja -C builddir-rel
ninja -C builddir-dbg
```

### Scenario 3: Development Workflow

```bash
# Terminal 1: Watch DPDK build
cd /tmp/dpdk && watch -n 1 "ls -l build/lib/libdpdk.* 2>/dev/null || echo 'Building...'"

# Terminal 2: Watch LTTT build with auto-recompile
cd /lttt && while true; do ninja -C builddir 2>&1 | tail -5; sleep 2; done
```

### Scenario 4: Docker/Container

```dockerfile
# Build DPDK in container
RUN git clone https://github.com/DPDK/dpdk.git /dpdk && \
    cd /dpdk && meson setup build && meson compile -C build

# Build LTTT using container DPDK
WORKDIR /lttt
RUN meson setup builddir -Ddpdk_build_dir=/dpdk/build && \
    ninja -C builddir
```

## Expected Directory Layout

```console
Your DPDK build:
/tmp/dpdk/build/
├── include/
│   ├── rte_config.h          ← REQUIRED
│   ├── rte_version.h
│   ├── rte_*.h               ← DPDK headers
│   └── ...
└── lib/
    ├── libdpdk.a             ← REQUIRED (or libdpdk.so)
    ├── libdpdk.so.xx.yy      ← If using shared
    └── ...
```

## Verify Configuration

Check what Meson sees:

```bash
# View all build options
meson configure builddir

# View just DPDK settings
meson configure builddir | grep -i dpdk

# Show which DPDK will be used (look for message)
meson setup builddir -Ddpdk_build_dir=/path 2>&1 | grep -i "dpdk\|found"
```

## Common Issues & Quick Fixes

| Issue | Solution |
|-------|----------|
| `DPDK build directory does not exist` | Check path is correct: `ls /path/to/dpdk/build/include/rte_config.h` |
| `libdpdk not found at link time` | Check lib exists: `ls /path/to/dpdk/build/lib/libdpdk.*` |
| `rte_config.h not found` | Verify build completed: Check `/path/to/dpdk/build/include/` has headers |
| Still using system DPDK | Check environment: `echo $RTE_SDK_BUILD` or use explicit Meson option |
| Build hangs or slow | DPDK build may still be running - wait for `meson compile` to complete |

## One-Liners

```bash
# Setup with installed DPDK
meson setup builddir && ninja -C builddir

# Setup with local DPDK
meson setup builddir -Ddpdk_build_dir=/tmp/dpdk/build && ninja -C builddir

# Setup with environment
export RTE_SDK_BUILD=/tmp/dpdk/build && meson setup builddir && meson compile -C builddir

# Clean rebuild
rm -rf builddir && meson setup builddir -Ddpdk_build_dir=/tmp/dpdk/build && meson compile -C builddir
```

## Getting Help

```bash
# Script help
make help

# Meson options
meson setup --help | grep -A 20 "project options"

# Read docs
cat NON_INSTALLED_DPDK.md
cat MESON_BUILD.md
cat DPDK_BUILD_CHANGES.md

# Check Meson version (need >= 1.5.0)
meson --version
```

## Summary: Three Key Points

1. **DPDK build directory must have:**
   - `include/` with `rte_config.h`
   - `lib/` with `libdpdk.a` or `libdpdk.so`

2. **Tell the build system where DPDK is (pick one method):**
   - `make USE_DPDK_SOURCE=true` (Makefile - easiest)
   - `-Ddpdk_build_dir=/path` (Meson option)
   - `export RTE_SDK_BUILD=/path` (environment)

3. **Then build normally:**
   - `ninja -C builddir`

Done! 🚀

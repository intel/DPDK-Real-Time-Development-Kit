# Real-Time Development Kit (RTDK)

<img src="./figures/launch-time.png" width="250" alt="LTTT Logo">

The Real-Time Development Kit (RTDK) is a comprehensive framework designed to measure network and compute performance of real-time applications across diverse environments. It provides developers with a suite of utilities and configurations to ensure their applications meet the stringent performance requirements of real-time systems.

RTDK comprises three core components: a primary DPDK application, an automated test case framework, and a Docker Compose configuration that enables statistical visualization through Grafana. While traditional DPDK applications are typically optimized for maximum throughput, RTDK distinguishes itself by prioritizing ultra-low latency performance.

The RTDK build system has been enhanced to automatically clone, patch, build, and install DPDK from source into a local directory within the project. This ensures that all developers use the same DPDK version with the necessary patches applied, eliminating discrepancies caused by differing system-installed DPDK versions.

The repository includes three main applications:

- **Real-time Application Testing Tool (RATT)**: A utility for real-time application testing and telemetry.
- **Launch-Time Testing Tool (LTTT)**: A DPDK-based application for precise packet transmission and reception timing.
- **TSN Testbench (TSN-TestBench)**: A Linux-based application for evaluating Time-Sensitive Networking (TSN) performance.

## Features

- **Comprehensive Platform Support**: RTDK operates seamlessly across multiple deployment environments, including bare metal servers, VMware virtualized infrastructure, and cloud platforms, ensuring consistent testing capabilities regardless of your infrastructure setup.
- **Streamlined Configuration Management**: Effortlessly create, manage, and deploy tailored configurations for diverse testing scenarios, enabling rapid adaptation to different performance requirements and test conditions.
- **Real-time Performance Monitoring & Diagnostics**: Continuously monitor application performance with visibility into system bottlenecks and optimization opportunities. RTDK's analytics help identify issues across the entire stack including:
- **Hardware-level problems**: High PCIe read latency, memory bandwidth constraints, etc,
- **Firmware misconfigurations**: ASPM (Active State Power Management) settings, power management policies, etc.
- **OS-level issues**: Incorrect CPU core isolation, sub-optimal interrupt handling, scheduler conflicts, etc.

## Getting Started

### Download and Install RTDK

1. Clone the repository:

   ```bash
   git clone https://github.com/intel-innersource/applications.dpdk.rt.rtdk.git rtdk
   cd rtdk
   ```

2. Build the application:

   The build system automatically clones, patches, builds, and installs DPDK from source into the `external/install` directory. This ensures you're using the correct DPDK version with all required patches applied.

   ```bash
   # Build everything (DPDK + RATT + Launch-Time + other apps)
   make

   # Debug build
   make BUILD_TYPE=debug ENABLE_DEBUG=true

   # Use specific DPDK version
   make distclean
   make DPDK_GIT_TAG=v24.11

   # Install LTTT (optional, requires sudo)
   make install
   ```

   **Note:** The first build will take longer as it clones and builds DPDK. Subsequent builds will be faster as DPDK is already built.

   **Alternative: Direct Meson/Ninja usage:**

   ```bash
   # First, ensure DPDK is installed in external/install
   make dpdk

   # Then build with Meson (PKG_CONFIG_PATH must point to external/install/lib/pkgconfig)
   export PKG_CONFIG_PATH=$PWD/external/install/lib/pkgconfig:$PKG_CONFIG_PATH
   meson setup builddir
   ninja -C builddir
   ```

   For detailed build instructions and Makefile options, see [doc/MAKEFILE.md](doc/MAKEFILE.md).
   For Meson-specific details, see [MESON_BUILD.md](MESON_BUILD.md).

3. Configure your testing environment by modifying the configuration files in the `configs/` directory.

4. **Run the applications**:

   This project includes three main applications:
   - **lttt** (Launch-Time Testing Tool) - located in `builddir/apps/launch-time/lttt`
   - **tsn_tb** (TSN Testbench) - located in `builddir/apps/testbench/tsn_tb`
   - **ratt** (RATT tool) - located in `builddir/apps/ratt/ratt`

   **Important**: DPDK applications require root privileges and proper library paths.

   ### Using the usertools/run Helper Script

   The easiest way to run applications is using the `usertools/run` helper script, which automatically:
   - Sets `LD_LIBRARY_PATH` to include the locally-built DPDK libraries
   - Executes the command with `sudo`
   - Locates applications in `builddir/apps` if not found in the current directory
   - Works from any directory in the project

   **Examples**:

   ```bash
   # Run by application name (script will find it in builddir/apps)
   ./usertools/run lttt --help
   ./usertools/run tsn_tb -c configs/example.cfg
   ./usertools/run ratt --help

   # Or use full paths if preferred
   ./usertools/run ./builddir/apps/launch-time/lttt --help
   ./usertools/run ./builddir/apps/testbench/tsn_tb -c configs/example.cfg

   # Works from any directory in the project
   cd usertools
   ./run lttt --help
   ```

   ### Running Without the Helper Script

   If you prefer to run applications directly, you must:
   1. Set the library path to the locally-built DPDK
   2. Run with sudo

   ```bash
   # Set LD_LIBRARY_PATH (from project root)
   export LD_LIBRARY_PATH=$PWD/external/install/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

   # Run with sudo
   sudo -E ./builddir/apps/launch-time/lttt --help
   sudo -E ./builddir/apps/testbench/tsn_tb --help
   ```

   Note: The `-E` flag preserves environment variables including `LD_LIBRARY_PATH`.

   ### Application-Specific Documentation

   For detailed usage instructions for each application, see:
   - Launch-Time Testing Tool: `apps/launch-time/README-launch-time.md`
   - TSN Testbench: `apps/testbench/README.md`
   - Configuration files: `configs/README-configs.md`

## Contributing

We welcome contributions from the community! If you have suggestions for improving LTTT or want to add new features, please submit a pull request.

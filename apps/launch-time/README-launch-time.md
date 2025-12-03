# Launch-Time Testing Tool (LTTT) Source

This directory contains the source code for the LTTT application.

## Source Files

- `launch-time.c` - Main application entry point
- `launch-time.h` - Main header with data structures and definitions
- `reference.c` - Reference mode implementation (packet transmission with timestamping)
- `mirror.c` - Mirror mode implementation (packet forwarding)
- `port-setup.c` - Ethernet port initialization and configuration
- `parse-args.c` - Command-line argument parsing
- `keyboard.c` - Keyboard input handling for interactive control
- `stats.c` / `stats.h` - Statistics collection and reporting
- `log.c` / `log.h` - Logging functionality
- `mqtt.c` / `mqtt.h` - MQTT client for telemetry publishing
- `consts.h` - Constants and macros

## Building

From the project root:

```bash
make
```

This will build DPDK (if needed) and all applications. The executable will be built as `builddir/apps/launch-time/lttt`.

## Running

**Recommended**: Use the helper script from the project root:

```bash
# Using the usertools/run helper (handles library paths, sudo, and locates the app automatically)
./usertools/run lttt [OPTIONS]

# View help
./usertools/run lttt --help

# Or use full path if preferred
./usertools/run ./builddir/apps/launch-time/lttt --help
```

**Alternative**: Run directly with proper environment:

```bash
export LD_LIBRARY_PATH=$PWD/external/install/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
sudo -E ./builddir/apps/launch-time/lttt [OPTIONS]
```

Use `--help` to see available options.

## Key Features

- **Hardware Timestamping**: Uses DPDK's RX timestamp offload for precise packet timing
- **Launch Time Control**: Uses DPDK's TX send-on-timestamp offload for scheduled transmission
- **Two Operating Modes**:
  - **Reference Mode** (default): Generates and transmits packets with precise timing
  - **Mirror Mode**: Receives and retransmits packets (loopback testing)
- **MQTT Integration**: Optional telemetry publishing to MQTT broker
- **Statistics Logging**: Detailed performance metrics collection

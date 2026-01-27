<!-- SPDX-License-Identifier: BSD-3-Clause -->
<!-- Copyright(c) 2025 Intel Corporation -->

# Launch-Time Testing Tool (LTTT) Source

This directory contains the source code for the LTTT application.

## Source Files

- `launch-time.c` - Main application entry point
- `launch-time.h` - Main header with data structures and definitions
- `rxtx.c` - TX/RX packet handling
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

## Command Line Options

### Required Arguments

- `-c | --launch-interval N` - Launch time interval in nanoseconds (e.g., 31250 = 31.25µs)
- `-b | --burst-length N` - Burst count and packet length in format `count/length` (e.g., `1/64` or `4/128`)

### Optional Arguments

**Network Configuration:**

- `-d | --dest-mac MAC` - Destination MAC address (default: FF:FF:FF:FF:FF:FF)
- `-s | --link-speed N` - Desired NIC link speed in Mbps (Default: Auto-negotiation)
- `-P | --promiscuous` - Enable promiscuous mode (Default: Disabled)

**Timing and Performance:**

- `-L | --launch-time` - Enable launch time support
- `-H | --hw-timestamp` - Enable hardware timestamping
- `-D | --delay-time N` - Startup delay in seconds (Default: 2)
- `-T | --tx-burst-offset N` - TX burst offset in ns before cycle end (Default: auto-calculated as 2% of launch interval, max 60µs)

**Runtime Control:**

- `-R | --run-duration N` - Run duration in format `Hours:Minutes:Seconds` (default: run forever)

**Logging and Telemetry:**

- `-l | --log-file FILE` - Log packet timestamps to FILE
- `-M | --mqtt` - Enable MQTT logging (Default: Disabled)

**Help:**

- `-h | --help` - Print help text and exit

## Usage Examples

### Basic Usage

Generate packets at 31.25µs intervals with 1 packet of 64 bytes per burst:

```bash
./usertools/run lttt -c 31250 -b 1/64
```

### With Launch Time and Hardware Timestamping

Enable precise launch time control and hardware timestamps:

```bash
./usertools/run lttt -c 31250 -b 1/64 -L -H
```

### Custom TX Burst Offset

Manually specify when to start transmitting before the cycle end:

```bash
./usertools/run lttt -c 31250 -b 1/64 -T 50000  # 50µs before cycle end
```

### With MQTT Telemetry and Logging

Enable MQTT for remote monitoring and log to file:

```bash
./usertools/run lttt -c 31250 -b 1/64 -M -l /var/log/lttt.log
```

### Fixed Link Speed

Run with a fixed 10Gbps link speed:

```bash
./usertools/run lttt -c 31250 -b 1/64 -s 10000
```

### Timed Run

Run for 1 hour with 5 second startup delay:

```bash
./usertools/run lttt -c 31250 -b 1/64 -R 1:00:00 -D 5
```

### Multiple Packets Per Burst

Send 4 packets of 128 bytes per burst (max 256):

```bash
./usertools/run lttt -c 62500 -b 4/128
```

## Key Features

- **Hardware Timestamping**: Uses DPDK's RX timestamp offload for precise packet timing
- **Launch Time Control**: Uses DPDK's TX send-on-timestamp offload for scheduled transmission
- **MQTT Integration**: Optional telemetry publishing to MQTT broker
- **Statistics Logging**: Detailed performance metrics collection

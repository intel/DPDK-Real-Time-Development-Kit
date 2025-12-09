# Real-time Application Testing Tool (RATT)

## About

RATT (Real-time Application Testing Tool) is a DPDK-based application designed to measure real-time network performance while optionally executing custom workloads. It supports two operating modes:

- **Reference Mode** (default): Generates and transmits packets at precise cycle times for timing analysis
- **Mirror Mode**: Receives and retransmits packets for loopback testing

RATT can integrate custom real-time workloads to evaluate their impact on network performance, making it ideal for testing real-time applications under realistic network load conditions.

## Features

- Precise cycle-time based packet generation and transmission
- Hardware timestamp support for accurate timing measurements
- Custom workload integration via shared libraries
- MQTT telemetry for remote monitoring
- Configurable burst sizes and packet lengths
- Link speed control
- Promiscuous mode support
- Statistics collection and logging

## Building

From the project root:

```bash
make
```

This will build DPDK (if needed) and all applications. The executable will be built as `builddir/apps/ratt/ratt`.

## Running

**Recommended**: Use the helper script from the project root:

```bash
# Using the usertools/run helper (handles library paths, sudo, and locates the app automatically)
./usertools/run ratt [OPTIONS]

# View help
./usertools/run ratt --help

# Or use full path if preferred
./usertools/run ./builddir/apps/ratt/ratt --help
```

**Alternative**: Run directly with proper environment:

```bash
export LD_LIBRARY_PATH=$PWD/external/install/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
sudo -E ./builddir/apps/ratt/ratt [OPTIONS]
```

## Command Line Options

### Required Arguments

- `-r | --reference` - Enable Reference mode (Default Enabled)
- `-m | --mirror` - Enable Mirror mode (Default Disabled)
- `-c | --cycle-time N` - Cycle time in nanoseconds (e.g., 31250 = 31.25µs)
- `-b | --burst-length N` - Burst count and packet length in format `count/length` (e.g., `1/64` or `4/128`)

### Optional Arguments

- `-d | --dest-mac MAC` - Destination MAC address (default: FF:FF:FF:FF:FF:FF)
- `-l | --log-file FILE` - Log packet timestamps to FILE
- `-M | --mqtt` - Enable MQTT logging (Default Disabled)
- `-D | --deltas` - Enable logging of time deltas (Default Disabled)
- `-s | --link-speed` - Desired NIC link speed in Mbps (Default: Auto-negotiation)
- `-R | --run-duration` - Run duration in format `Hours:Minutes:Seconds` (default: forever)
- `-P | --promiscuous` - Enable promiscuous mode (Default Disabled)
- `-S | --mirror-serial` - Serialize packets in mirror mode (Default Disabled)
- `-i | --internal-debug` - Display internal debugging statistics
- `-h | --help` - Print help text
- `-w | --workload` - Real-time workload specification (see Workload Integration below)
- `--skip-count` - Number of initial packets to skip (Default: 5)
- `--delay-time` - Startup delay in seconds (Default: 0)
- `--continue-on-err` - Continue running on timing validation errors (Default Disabled)
- `--hw-timestamp` - Enable hardware timestamping (if supported)

## Usage Examples

### Basic Reference Mode

Generate packets at 31.25µs cycle time with 1 packet of 64 bytes per burst:

```bash
./usertools/run ratt -c 31250 -b 1/64
```

### Mirror Mode

Receive and retransmit packets:

```bash
./usertools/run ratt --mirror -c 31250 -b 1/64
```

### With MQTT Telemetry

Enable MQTT logging for remote monitoring:

```bash
./usertools/run ratt -c 31250 -b 1/64 --mqtt
```

### Fixed Link Speed

Run with a fixed 10Gbps link speed:

```bash
./usertools/run ratt -c 31250 -b 1/64 -s 10000
```

### Timed Run

Run for 1 hour:

```bash
./usertools/run ratt -c 31250 -b 1/64 -R 1:00:00
```

## Workload Integration

RATT can execute custom real-time workloads during packet processing to evaluate their impact on network performance.

### Workload Specification

Use the `-w | --workload` option with the format:

```console
filename,function,parameters
```

- **filename**: Path to the shared library (.so file)
- **function**: Name of the function to call
- **parameters**: Optional comma-separated parameters to pass to the function

### Creating a Workload

1. **Write your workload function** with the signature:

```c
int my_workload_function(int argc, char **argv);
```

2. **Compile as a shared library**:

```bash
gcc -shared -fPIC -o my_workload.so my_workload.c
```

3. **Run RATT with your workload**:

```bash
./usertools/run ratt -c 31250 -b 1/64 -w ./my_workload.so,my_workload_function,param1,param2
```

### Example Workload

```c
#include <stdio.h>

int compute_intensive_task(int argc, char **argv) {
    // Perform some computation
    volatile int result = 0;
    for (int i = 0; i < 10000; i++) {
        result += i;
    }
    return 0;
}
```

Compile and use:

```bash
gcc -shared -fPIC -o workload.so workload.c
./usertools/run ratt -c 31250 -b 1/64 -w ./workload.so,compute_intensive_task
```

## Statistics and Monitoring

RATT provides real-time statistics including:

- Packet transmission/reception rates
- Timing deltas and jitter
- Port statistics (errors, missed packets)
- Workload execution metrics

Press `s` during runtime to display statistics, `q` to quit.

## Notes

- RATT requires root privileges to access hardware resources
- Hardware timestamping support depends on NIC capabilities
- For best real-time performance, ensure proper CPU core isolation and IRQ affinity
- MQTT telemetry requires a running MQTT broker (e.g., Mosquitto)

## See Also

- Main project README: `../../README.md`
- Configuration examples: `../../configs/README-configs.md`
- Launch-Time Testing Tool: `../launch-time/README-launch-time.md`
- TSN Testbench: `../testbench/README.md`

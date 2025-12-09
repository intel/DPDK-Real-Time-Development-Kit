# TSN Testbench Application (tsn_tb)

## About

The TSN Testbench (`tsn_tb`) is a DPDK-based application for evaluating Time-Sensitive Networking (TSN) performance on Linux systems. Originally developed by Linutronix, this version has been ported to DPDK for enhanced performance and flexibility.

The application is designed to simulate industrial real-time protocols like PROFINET and OPC/UA PubSub, making it ideal for validating TSN implementations and evaluating hardware performance.

## Operating Modes

The TSN Testbench supports two primary modes:

### Reference Mode (Default)

- Generates cyclic real-time traffic at precise intervals
- Performs timing validation and consistency checks
- Simulates PLC (Programmable Logic Controller) behavior
- Measures latency, jitter, and throughput

### Mirror Mode

- Receives and retransmits packets for loopback testing
- Used in conjunction with a reference instance
- Validates round-trip timing and packet integrity

## Features

- **Multi-Protocol Support**: PROFINET, OPC/UA PubSub, and custom Ethernet payloads
- **Configurable Traffic Classes**: TSN High, TSN Low, RTA (Real-Time Application), RTC (Real-Time Communication), DCP, LLDP, UDP
- **Hardware Timestamping**: Precise packet timing using NIC capabilities
- **Launch Time Control**: Tx time-based packet scheduling for deterministic transmission
- **PTP Synchronization**: IEEE 1588/802.1AS time synchronization
- **Security Support**: Real-time encryption with AES256 and other algorithms
- **MQTT Telemetry**: Remote monitoring and logging
- **Flexible Configuration**: YAML-based configuration files
- **CPU Affinity Control**: Thread pinning for real-time performance

## Building

From the project root:

```bash
make
```

This will build DPDK (if needed) and all applications. The executable will be built as `builddir/apps/testbench/tsn_tb`.

## Running

**Recommended**: Use the helper script from the project root:

```bash
# Using the usertools/run helper (handles library paths, sudo, and locates the app automatically)
./usertools/run tsn_tb [OPTIONS]

# View help
./usertools/run tsn_tb --help

# Run with configuration file (reference mode)
./usertools/run tsn_tb -c apps/testbench/configs/reference-T1000.yaml

# Run in mirror mode
./usertools/run tsn_tb -m -c apps/testbench/configs/mirror-T1000.yaml

# Or use full path if preferred
./usertools/run ./builddir/apps/testbench/tsn_tb --help
```

**Alternative**: Run directly with proper environment:

```bash
export LD_LIBRARY_PATH=$PWD/external/install/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
sudo -E ./builddir/apps/testbench/tsn_tb [OPTIONS]
```

## Command Line Options

- `-c | --config <filename>` - Path to YAML configuration file (required)
- `-m | --mirror` - Start in mirror mode (default: reference mode)
- `-V | --version` - Print version information
- `-h | --help` - Print help text

## Configuration Files

The testbench uses YAML configuration files located in `apps/testbench/configs/`. These files define:

- Cycle times and timing offsets
- Traffic classes and their parameters
- Thread priorities and CPU affinity
- Logging and telemetry settings
- Frame sizes and payload patterns
- VLAN IDs and MAC addresses

### Available Configurations

**Reference Mode Configurations:**

- `reference-15.625us.yaml` - 15.625µs cycle time (64kHz)
- `reference-31.250us.yaml` - 31.25µs cycle time (32kHz)
- `reference-T1000.yaml` - 1ms cycle time (standard PROFINET)
- `reference-T2000.yaml` - 2ms cycle time
- `reference-T0125.yaml` - 125µs cycle time
- `reference-T0250.yaml` - 250µs cycle time
- `reference-T0500.yaml` - 500µs cycle time

**Mirror Mode Configurations:**

- `mirror-15.625us.yaml` - Mirror for 15.625µs cycle
- `mirror-T1000.yaml` - Mirror for 1ms cycle
- `mirror-T2000.yaml` - Mirror for 2ms cycle
- Additional mirror configurations for various cycle times

### Configuration Structure

Key configuration sections:

```yaml
Application:
  ApplicationBaseCycleTimeNS: 1000000    # 1ms cycle time
  ApplicationTxBaseOffsetNS: 800000      # TX offset from cycle start
  ApplicationRxBaseOffsetNS: 600000      # RX offset from cycle start

Log:
  LogFile: /var/log/reference-T1000.log
  LogLevel: Debug

TSNHigh:                                  # High-priority TSN traffic
  TsnHighEnabled: true
  TsnHighNumFramesPerCycle: 128
  TsnHighFrameLength: 128
  TsnHighVid: 100
  TsnHighLPortID: 0:0

TSNLow:                                   # Low-priority TSN traffic
  TsnLowEnabled: false

RTA:                                      # Real-Time Application class
  RtaEnabled: false

UDP:                                      # Non-real-time UDP traffic
  UdpEnabled: false
```

## Usage Examples

### Basic PROFINET T=1ms Test

**Reference instance:**

```bash
./usertools/run tsn_tb -c apps/testbench/configs/reference-T1000.yaml
```

**Mirror instance (on separate system or port):**

```bash
./usertools/run tsn_tb -m -c apps/testbench/configs/mirror-T1000.yaml
```

### Ultra-Fast Cycle Time (15.625µs)

For high-performance industrial applications:

```bash
# Reference
./usertools/run tsn_tb -c apps/testbench/configs/reference-15.625us.yaml

# Mirror
./usertools/run tsn_tb -m -c apps/testbench/configs/mirror-15.625us.yaml
```

### Custom Configuration

1. Copy an existing configuration:

```bash
cp apps/testbench/configs/reference-T1000.yaml my-config.yaml
```

2. Edit the configuration to match your requirements

3. Run with your custom configuration:

```bash
./usertools/run tsn_tb -c my-config.yaml
```

## Test Setup

### Two-System Setup (Recommended)

```console
┌──────────────┐         ┌──────────────┐
│  System A    │         │  System B    │
│              │ Ethernet│              │
│  Reference   ├─────────┤   Mirror     │
│  (tsn_tb)    │         │   (tsn_tb)   │
└──────────────┘         └──────────────┘
```

### Single-System Loopback

Use two ports on the same system connected via a cable or switch.

## Performance Tuning

For optimal real-time performance:

1. **Isolate CPU cores** using kernel boot parameters:

   ```console
   isolcpus=17,18,19
   ```

2. **Configure IRQ affinity** to non-isolated cores

3. **Disable power management**:

   ```bash
   sudo cpupower frequency-set -g performance
   ```

4. **Use real-time kernel** (PREEMPT_RT patch)

5. **Configure PTP** for time synchronization:

   ```bash
   sudo scripts/ptp.sh eth0 # Start PTP on eth0 or i226 interface
   ```

6. **Set thread priorities** in YAML configuration files

## Monitoring and Statistics

The testbench provides detailed runtime statistics including:

- **Timing Metrics**: Minimum, maximum, and average latencies
- **Packet Statistics**: Transmitted, received, dropped, and error counts
- **Jitter Analysis**: Cycle-to-cycle timing variation
- **Throughput**: Data rates per traffic class
- **Error Detection**: Sequence errors, late packets, timestamp violations

Statistics are logged to the configured log file and can be exported via MQTT for remote monitoring.

## MQTT Integration

Enable MQTT telemetry for real-time monitoring:

```yaml
LogViaMQTT:
   LogViaMQTT: True
   LogViaMQTTBrokerIP: 127.0.0.1
   LogViaMQTTBrokerPort: 1883
   LogViaMQTTMeasurementName: reference
```

This allows integration with monitoring tools like Grafana and InfluxDB.

## Troubleshooting

### Common Issues

**Issue**: Application fails to start

- **Solution**: Ensure you have root privileges and the port is not in use

**Issue**: High jitter or missed cycles

- **Solution**: Check CPU isolation, IRQ affinity, and power management settings

**Issue**: PTP synchronization fails

- **Solution**: Verify network cable, PTP daemon configuration, and NIC support

**Issue**: Build errors for BPF

- **Solution**: Install required packages:

  ```bash
  sudo apt-get install g++-multilib libc6-dev-i386
  ```

### Debug Mode

Enable detailed logging by setting `LogLevel: Debug` in the configuration file.

## Requirements

- **Hardware**: TSN-capable NIC (e.g., Intel i225, i226)
- **Software**: DPDK support, PTP daemon (ptp4l)
- **OS**: Linux (preferably with PREEMPT_RT patch)
- **Privileges**: Root access for hardware control

## See Also

- Original project: <https://github.com/Linutronix/TSN-Testbench>
- Documentation: <https://linutronix.github.io/TSN-Testbench>
- Main README: `README.md` (project root)
- RATT: `../ratt/README-ratt.md`
- Launch-Time Testing Tool: `../launch-time/README-launch-time.md`
- Configuration guide: `../../configs/README-configs.md`

## Credits

- Original development: Linutronix GmbH
- Idea and initial funding: Phoenix Contact Electronics GmbH
- Supported by: Siemens AG and Intel Corporation

## License

BSD-2-Clause and Dual BSD/GPL for eBPF programs

Copyright (C) 2020-2024 Linutronix GmbH
Copyright (C) 2024 Intel Corporation

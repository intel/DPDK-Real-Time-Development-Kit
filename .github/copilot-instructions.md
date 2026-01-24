# Copilot instructions (RTDK)

## Project shape
- RTDK is a DPDK-based toolkit with multiple apps built via Meson/Ninja under `builddir/`.
- Apps live under `apps/`: `launch-time/` (LTTT), `ratt/` (RATT), `testbench/` (TSN Testbench), `cnp/`.
- DPDK is vendored as a reproducible dependency: cloned to `external/dpdk/`, patched from `patches/`, installed to `external/install/`.

## Build workflow (preferred)
- Build everything (includes cloning/patching/building DPDK): `make`
- Debug build + extra logging flags: `make BUILD_TYPE=debug ENABLE_DEBUG=true`
- Rebuild from scratch including DPDK: `make distclean && make`
- Switch DPDK version: `make distclean DPDK_GIT_TAG=v24.11 && make`

Notes for agents:
- The Makefile exports `PKG_CONFIG_PATH` pointing at `external/install/lib/x86_64-linux-gnu/pkgconfig` so Meson finds `libdpdk`.
- `meson_options.txt` defines project knobs used across apps: `enable_mqtt`, `enable_debug`, `max_lcores`.

## Running (canonical)
- DPDK apps typically require root + the locally installed DPDK libs.
- Prefer the helper script which sets `LD_LIBRARY_PATH`, locates binaries under `builddir/apps`, and runs with sudo:
  - `./usertools/run lttt --help`
  - `./usertools/run ratt --help`
  - `./usertools/run tsn_tb -c configs/example.cfg`
- If running directly, preserve env vars: `sudo -E ./builddir/apps/launch-time/lttt ...`

## DPDK patching conventions
- Do not hand-edit `external/dpdk/` for product changes; add/adjust patch files in `patches/`.
- The Makefile drops a marker file in the DPDK repo (`external/dpdk/.tsn_tb_patches_applied`) to avoid reapplying patches.

## Codebase patterns to follow
- Apps share a common C module layout; when adding features, look for and mirror existing structure:
  - CLI parsing in `parse-args.c`
  - Port/NIC init in `port-setup.c`
  - TX/RX loop in `rxtx.c`
  - Interactive control in `keyboard.c`
  - Stats in `stats.c`/`stats.h`
  - Logging in `log.c`/`log.h`, optional telemetry in `mqtt.c`/`mqtt.h` (guarded by `enable_mqtt` / `DISABLE_MQTT`).

## RATT workload integration (repo-specific)
- RATT can call a function from a user-provided shared library via `-w/--workload`.
- Expected workload symbol signature (per docs): `int my_workload_function(int argc, char **argv);`
- Invocation format: `./usertools/run ratt ... -w ./workload.so,my_workload_function,arg1,arg2`

## Where to look first
- Build + dependency bootstrapping: [Makefile](../Makefile), [meson.build](../meson.build)
- App entrypoints + module boundaries: `apps/launch-time/launch-time.c`, `apps/ratt/ratt.c`, `apps/testbench/*`
- Run helper: `usertools/run`
- High-level usage: [README.md](../README.md), app docs in `apps/*/README*.md`

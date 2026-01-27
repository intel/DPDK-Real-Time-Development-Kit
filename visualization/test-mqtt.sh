#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
#
# Publish simulated RATT data through the full visualization pipeline:
#   mosquitto_pub -> Mosquitto -> Telegraf -> InfluxDB -> Grafana
#
# Produces the same JSON format as apps/ratt/mqtt.c so every dashboard
# panel is exercised (rtdk-stats + rtdk-deltas measurements).
#
# Usage:
#   ./test-mqtt.sh              # 60 seconds, default values
#   ./test-mqtt.sh -d 30        # 30 seconds
#   ./test-mqtt.sh -d 120 -p 2000 -m 5000 -M 25000
#   ./test-mqtt.sh --help

set -euo pipefail

# ── defaults ────────────────────────────────────────────────────────
DURATION=60          # seconds of data to publish
RTT_MIN_BASE=8000    # nanoseconds – baseline RTT minimum
RTT_MAX_CEIL=25000   # nanoseconds – ceiling for RTT maximum
PPS_BASE=1000        # packets-per-second baseline
DELTAS_PER_SEC=10    # individual RTT samples per stats message
MQTT_TOPIC="rtdk-data"
CONTAINER="mosquitto"

# ── usage ───────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Publish simulated RATT telemetry to the visualization stack via MQTT.

Options:
  -d, --duration SEC       Duration in seconds (default: $DURATION)
  -m, --rtt-min  NS        Baseline RTT minimum in ns (default: $RTT_MIN_BASE)
  -M, --rtt-max  NS        Ceiling for RTT maximum in ns (default: $RTT_MAX_CEIL)
  -p, --pps      N         Packets-per-second baseline (default: $PPS_BASE)
  -n, --deltas   N         Delta samples per second (default: $DELTAS_PER_SEC)
  -t, --topic    TOPIC     MQTT topic (default: $MQTT_TOPIC)
  -c, --container NAME     Mosquitto container name (default: $CONTAINER)
  -h, --help               Show this help

Examples:
  $(basename "$0")                        # 60s with defaults
  $(basename "$0") -d 30                  # 30s burst
  $(basename "$0") -d 120 -p 2000        # 2 min at ~2000 pps
  $(basename "$0") -M 50000              # simulate higher RTT jitter
EOF
    exit 0
}

# ── parse args ──────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--duration)   DURATION="$2";      shift 2 ;;
        -m|--rtt-min)    RTT_MIN_BASE="$2";  shift 2 ;;
        -M|--rtt-max)    RTT_MAX_CEIL="$2";  shift 2 ;;
        -p|--pps)        PPS_BASE="$2";      shift 2 ;;
        -n|--deltas)     DELTAS_PER_SEC="$2"; shift 2 ;;
        -t|--topic)      MQTT_TOPIC="$2";    shift 2 ;;
        -c|--container)  CONTAINER="$2";     shift 2 ;;
        -h|--help)       usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# ── preflight checks ───────────────────────────────────────────────
COMPOSE_PROJECT=$(docker compose ls --format json 2>/dev/null | python3 -c "
import json, sys
for p in json.load(sys.stdin):
    if 'visualization' in p.get('ConfigFiles', ''):
        print(p['Name']); break
" 2>/dev/null || true)

if [[ -z "$COMPOSE_PROJECT" ]]; then
    # Fall back to directory-based name
    COMPOSE_PROJECT="rt-stats"
fi

NETWORK="${COMPOSE_PROJECT}_stats"

if ! docker network inspect "$NETWORK" &>/dev/null; then
    echo "Error: Docker network '$NETWORK' not found. Is the stack running?"
    echo "  cd visualization && docker compose up -d"
    exit 1
fi

# ── helpers ─────────────────────────────────────────────────────────
rand_range() {
    # Return a random integer in [min, max)
    local min=$1 max=$2
    echo $(( min + RANDOM % (max - min) ))
}

# Start a long-lived helper container on the compose network for publishing.
# The mosquitto image includes mosquitto_pub but the broker container itself
# doesn't expose it conveniently, so we run a disposable "sidecar".
HELPER_NAME="rtdk-mqtt-test-$$"
cleanup() { docker rm -f "$HELPER_NAME" &>/dev/null || true; }
trap cleanup EXIT

docker run -d --rm \
    --name "$HELPER_NAME" \
    --network "$NETWORK" \
    eclipse-mosquitto:2 \
    sh -c "sleep infinity" >/dev/null

# Batch all per-second publishes into a single docker exec for speed.
publish_batch() {
    docker exec "$HELPER_NAME" sh -c "$1"
}

# ── main loop ───────────────────────────────────────────────────────
echo "Publishing simulated RATT data for ${DURATION}s..."
echo "  RTT range : ${RTT_MIN_BASE}–${RTT_MAX_CEIL} ns"
echo "  PPS base  : ${PPS_BASE}"
echo "  Deltas/sec: ${DELTAS_PER_SEC}"
echo "  Topic     : ${MQTT_TOPIC}"
echo ""

TOTAL_STATS=0
TOTAL_DELTAS=0

for (( i=1; i<=DURATION; i++ )); do
    NOW_NS=$(date +%s%N)

    # Randomize within configured bounds
    RTT_MIN=$(rand_range "$RTT_MIN_BASE" $(( RTT_MIN_BASE + RTT_MIN_BASE / 4 )) )
    RTT_AVG=$(rand_range $(( RTT_MIN + 1000 )) $(( RTT_MIN + (RTT_MAX_CEIL - RTT_MIN_BASE) / 2 )) )
    RTT_MAX=$(rand_range $(( RTT_AVG + 1000 )) "$RTT_MAX_CEIL" )
    PPS=$(rand_range $(( PPS_BASE - PPS_BASE / 10 )) $(( PPS_BASE + PPS_BASE / 10 )) )

    TOTAL_RX=$(( PPS * i ))

    # Build a batch script: one stats publish + N delta publishes in a single docker exec
    BATCH="mosquitto_pub -h ${CONTAINER} -t ${MQTT_TOPIC} -m '"
    BATCH+="{\"reference\":{\"Timestamp\":${NOW_NS},\"MeasurementName\":\"rtdk-stats\",\"stats\":{"
    BATCH+="\"rttMinNs\":${RTT_MIN},\"rttMaxNs\":${RTT_MAX},"
    BATCH+="\"rttSumNs\":$(( RTT_AVG * PPS )),\"rttCount\":${PPS},\"rttAvgNs\":${RTT_AVG},"
    BATCH+="\"snapRttMinNs\":${RTT_MIN},\"snapRttMaxNs\":${RTT_MAX},"
    BATCH+="\"snapRttSumNs\":$(( RTT_AVG * PPS )),\"snapRttCount\":${PPS},\"snapRttAvgNs\":${RTT_AVG},"
    BATCH+="\"spikeRttMinNs\":0,\"spikeRttMaxNs\":0,\"spikeRttSumNs\":0,\"spikeRttCount\":0,\"spikeRttAvgNs\":0,"
    BATCH+="\"rxSnapshotMinNs\":0,\"rxSnapshotMaxNs\":0,\"rxSnapshotSumNs\":0,\"rxSnapshotCount\":0,\"rxSnapshotAvgNs\":0,"
    BATCH+="\"txSnapshotMinNs\":0,\"txSnapshotMaxNs\":0,\"txSnapshotSumNs\":0,\"txSnapshotCount\":0,\"txSnapshotAvgNs\":0,"
    BATCH+="\"rxPPS\":${PPS},\"txPPS\":${PPS},"
    BATCH+="\"noMBufs\":0,\"noTimestamp\":0,\"idError\":0,\"txRingFull\":0,"
    BATCH+="\"rxTimeout\":0,\"rxTryExtraTime\":0,"
    BATCH+="\"totalPktsRx\":${TOTAL_RX},\"totalPktsTx\":${TOTAL_RX},"
    BATCH+="\"ipackets\":${TOTAL_RX},\"opackets\":${TOTAL_RX},"
    BATCH+="\"ibytes\":$(( TOTAL_RX * 64 )),\"obytes\":$(( TOTAL_RX * 64 )),"
    BATCH+="\"imissed\":0,\"ierrors\":0,\"oerrors\":0,\"rxNoMbuf\":0"
    BATCH+="}}}'"
    TOTAL_STATS=$(( TOTAL_STATS + 1 ))

    # Append delta publishes
    DELTA_INTERVAL=$(( 1000000000 / DELTAS_PER_SEC ))
    for (( j=0; j<DELTAS_PER_SEC; j++ )); do
        DELTA=$(rand_range "$RTT_MIN" "$RTT_MAX")
        DELTA_TS=$(( NOW_NS + j * DELTA_INTERVAL ))
        BATCH+=" && mosquitto_pub -h ${CONTAINER} -t ${MQTT_TOPIC} -m '"
        BATCH+="{\"reference\":{\"Timestamp\":${DELTA_TS},\"MeasurementName\":\"rtdk-deltas\","
        BATCH+="\"stats\":{\"info\":\"dtag\",\"deltas\":${DELTA}}}}'"
        TOTAL_DELTAS=$(( TOTAL_DELTAS + 1 ))
    done

    publish_batch "$BATCH"

    printf "\r  %d/%d seconds  [stats: %d, deltas: %d]" \
        "$i" "$DURATION" "$TOTAL_STATS" "$TOTAL_DELTAS"
    sleep 1
done

echo ""
echo ""
echo "Done. Published ${TOTAL_STATS} stats + ${TOTAL_DELTAS} delta points."
echo "Open Grafana at http://localhost:3000 and set time range to 'Last 5 minutes'."

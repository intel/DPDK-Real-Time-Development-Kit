#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
#
# Create a DBRP mapping so InfluxQL queries using the legacy "database" name
# are routed to the v2 bucket. This keeps existing Grafana dashboards working.

BUCKET=${DOCKER_INFLUXDB_INIT_BUCKET:-rtdk}
ORG=${DOCKER_INFLUXDB_INIT_ORG:-rtdk}

# Get the bucket ID
BUCKET_ID=$(influx bucket list --name "$BUCKET" --org "$ORG" --json | \
    grep -o '"id"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | grep -o '"[^"]*"$' | tr -d '"')

if [ -z "$BUCKET_ID" ]; then
    echo "WARNING: Could not find bucket '$BUCKET' — skipping DBRP mapping"
    exit 0
fi

# Create the DBRP mapping (database name = bucket name, retention policy = default)
influx v1 dbrp create \
    --db "$BUCKET" \
    --rp "autogen" \
    --bucket-id "$BUCKET_ID" \
    --org "$ORG" \
    --default || true

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <stdint.h>
// Total size: 28 bytes (4 + 4*6)
// We align it to 8-byte boundaries just in case, though 4-byte is fine.

typedef struct {
    uint16_t magic;                  // Magic number to detect probe
    uint16_t packet_type;            // 1 for Probe_Send, 2 for Probe_Response
    uint32_t sequence_number;        // Unique ID for this probe cycle
    uint64_t T1;                     // Timestamp in nanoseconds
} __attribute__((packed)) probe_payload_t;

enum { TYPE_PROBE_SEND = 1, TYPE_PROBE_RECV };

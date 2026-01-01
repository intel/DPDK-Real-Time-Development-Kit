/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#pragma once

#include <stdint.h>

typedef struct {
    uint64_t min_ns;        // Minimum time in nanoseconds
    uint64_t max_ns;        // Maximum time in nanoseconds
    uint64_t avg_ns;        // Average time in nanoseconds
    uint64_t sum_ns;        // Total time for average in nanoseconds
    uint64_t count;         // Number of times
} min_avg_max_t;

typedef struct {
    uint64_t rx;
    uint64_t tx;
} pkt_count_t;

// Values in nanoseconds unless otherwise specified
typedef struct stats_s {
    pkt_count_t total_pkts;             // Total Rx/Tx reference packets
    uint64_t rx_pps;                    // Number of packets per second
    uint64_t tx_pps;                    // Number of packets per second
    uint64_t no_mbufs;                  // No mbufs available
	uint64_t many_rx;                   // Number of times more than 1 packet received in burst
    uint64_t rx_frame_errors;           // Number of Rx Error frames
    uint64_t rx_unknown_frames;         // Number of unknown Rx frames
    uint64_t rx_no_probe_frames;        // Number of Rx frames without probe data
    uint64_t tx_frame_errors;           // Number of Tx Error frames
    min_avg_max_t rtt;                  // Round Trip Time statistics
    min_avg_max_t hw_rtt;               // Hardware Round Trip Time statistics
} stats_t;

void print_stats(void);
void reset_stats(void);

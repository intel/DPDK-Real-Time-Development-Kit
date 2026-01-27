/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef _STATS_H_
#define _STATS_H_

#include <stdint.h>

typedef struct {
    uint64_t min_ns;        // Minimum time in nanoseconds
    uint64_t max_ns;        // Maximum time in nanoseconds
    uint64_t avg_ns;        // Average time in nanoseconds
    uint64_t sum_ns;        // Total RTT time for average in nanoseconds
    uint64_t count;         // Number of times
} min_avg_max_t;

typedef struct {
    uint64_t rx;
    uint64_t tx;
} pkt_count_t;

// Values in nanoseconds unless otherwise specified
typedef struct stats_s {
    min_avg_max_t rtt;                 // Round trip time statistics
    min_avg_max_t spike;               // RTT spike statistics
    min_avg_max_t snapshot;            // Snapshot RTT statistics
    min_avg_max_t rx_snapshot;         // Snapshot Rx statistics
    min_avg_max_t tx_snapshot;         // Snapshot Tx statistics
    min_avg_max_t workload;            // Workload statistics
    min_avg_max_t workload_snapshot;   // Snapshot workload statistics
    pkt_count_t total_pkts;            // Total Rx/Tx reference packets
    uint64_t rx_pps;                   // Number of packets per second
    uint64_t tx_pps;                   // Number of packets per second
    uint64_t no_mbufs;                 // No mbufs available
    uint64_t no_timestamp;             // No timestamp available
    uint64_t id_error;                 // Number of errors in packet ID
    uint64_t tx_ring_full;             // Number of times the transmit ring is full
    uint64_t rx_timeout;               // Number of times the receive operation timed out
    uint64_t rx_try_extra_time;        // Extra time to wait for Rx to complete
    uint64_t prev_rx_timeout;          // Previous Rx timeout time
} stats_t;

typedef struct {
    uint64_t timestamp;        // Timestamp in nanoseconds
    uint16_t beef;             // Beef field
    uint16_t id;               // Packet ID
    uint32_t reserved;         // Reserved for future use
} timestamp_t;

void print_stats(void);

#endif /* _STATS_H_ */

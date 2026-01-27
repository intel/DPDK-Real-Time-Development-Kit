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
    uint64_t sum_ns;        // Total time for average in nanoseconds
    uint64_t count;         // Number of times
} min_avg_max_t;

typedef struct {
    uint64_t rx;
    uint64_t tx;
} pkt_count_t;

// Values in nanoseconds unless otherwise specified
typedef struct stats_s {
    min_avg_max_t launch_time;         // Hardware launch time statistics
    pkt_count_t total_pkts;            // Total Rx/Tx packets
    uint64_t rx_pps;                   // Number of packets per second
    uint64_t tx_pps;                   // Number of packets per second
    uint64_t no_mbufs;                 // No mbufs available
} stats_t;

void print_stats(void);
void reset_rx_timestamp(void);

#endif /* _STATS_H_ */

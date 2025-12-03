/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#ifndef _RTC_CONST_H_
#define _RTC_CONST_H_

#include <rte_time.h>

#ifndef CLOCK_TAI
#define CLOCK_TAI 1
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 0
#endif

#define HAS_HW_TIMESTAMPING 0        // Disabled for now

enum {
    THE_BEEF_TIMESTAMP = 0xbeef,            // Magic value for BEEF timestamp
    BIG_NUM            = 1000000,           // A big number
    MAX_BURST_COUNT    = (2 * 1024),        // Maximum number of packets to send per burst
    DEFAULT_RING_SIZE  = (4 * 1024),        // Size of ring for each port
    DEFAULT_MBUF_COUNT = (32 * 1024),       // Number of mbufs per port
    DEFAULT_CACHE_SIZE = 128,               // Size of cache per pktmbuf pool
    DEFAULT_PRIV_SIZE  = 0,                 // Size of private data per pktmbuf pool
    MIN_PKT_LENGTH     = 64,                // Minimum packet length
    MAX_PKT_LENGTH     = 1536,              // Maximum packet length
    FCS_SIZE           = 4,                 // Size of FCS
    TIMESTAMP_OFFSET   = 32,                // Offset of timestamp in bytes
    DEFAULT_DELAY_SEC  = 2,                 // Default delay in seconds before Tx packets
    DEFAULT_SKIP_COUNT = 10,                // Number of packets to throw away
    USEC_PER_SEC       = (NSEC_PER_SEC / 1000UL),        // Number of microseconds in a second
};

#endif

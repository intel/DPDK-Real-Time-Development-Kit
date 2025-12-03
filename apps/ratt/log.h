/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#ifndef _LOG_H_
#define _LOG_H_

#include "ratt.h"

enum {
    LOG_RING_SIZE         = 2048,               // Size of log ring for statistics
    LOG_BUFFER_POOL_COUNT = (4 * 1024),         // Number of log buffers in pool
    LOG_BUFFER_SIZE       = 2048,               // Size of each buffer in log memory pool
    LOG_BURST_COUNT       = 256,
};

typedef struct log_statistics {
    uint64_t time_stamp; /* Time stamp in nanoseconds. */
    uint64_t delta_ns;   /* Delta time in nanoseconds. */
    uint64_t rtt_min;    /* Minimum round trip time in nanoseconds. */
    uint64_t rtt_max;    /* Maximum round trip time in nanoseconds. */
    uint64_t rtt_avg;    /* Average round trip time in nanoseconds. */
} log_statistics_t;

int pow2roundup(u_int32_t x);
int log_init(char *logfile);
void log_close(void);
int log_open(void);
void log_message(const char *format, ...);
void log_delta(uint64_t delta);
void log_flush(void);

#endif

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef _LOG_H_
#define _LOG_H_

#include "launch-time.h"

enum {
    LOG_RING_SIZE         = 2048,              // Size of log ring for statistics
    LOG_BUFFER_POOL_COUNT = (4 * 1024),        // Number of log buffers in pool
    LOG_BUFFER_SIZE       = 2048,              // Size of each buffer in log memory pool
    LOG_BURST_COUNT       = 256,
};

int log_init(const char *logfile);
void log_close(void);
int log_open(void);
void log_message(const char *format, ...);
void log_flush(void);

#endif /* _LOG_H_ */
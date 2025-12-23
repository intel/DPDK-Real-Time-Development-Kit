/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#ifndef _MQTT_H_
#define _MQTT_H_

#include "cnp-tuning.h"

enum {
    LOG_MQTT_BUFFER_COUNT = 256,            // Number of buffers in MQTT memory pool
    LOG_MQTT_BUFFER_SIZE  = 4096,           // Size of each buffer in MQTT memory pool
    LOG_MQTT_PERIOD_NS    = 1000000,        // Period for MQTT logging in nanoseconds
};

typedef struct mqtt_statistics {
    uint64_t timestamp;              /* Time stamp in nanoseconds. */
    stats_t stats;                   /* Statistics for the MQTT client. */
    struct rte_eth_stats port_stats; /* Port statistics for the MQTT client. */
} mqtt_statistics_t;

int mqtt_init(void);
void mqtt_stats(stats_t *stats, struct rte_eth_stats *port_stats);
void mqtt_close(void);

int mqtt_thread_routine(void *data __rte_unused);

#endif

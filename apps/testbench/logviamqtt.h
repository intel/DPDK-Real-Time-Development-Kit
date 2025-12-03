/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2024 Intel Corporation.
 * Author Walfred Tedeschi <walfred.tedeschi@intel.com>
 */

#ifndef _LOGVIAMQTT_H_
#define _LOGVIAMQTT_H_

#include <mosquitto.h>

#include <rte_common.h>
#include <rte_mempool.h>
#include <rte_ring.h>

enum {
	LOG_MQTT_RING_SIZE = 1024,
	LOG_MQTT_MEMPOOL_BUFFER_COUNT = (8 * 1024),
	LOG_MQTT_MEMPOOL_BUFFER_SIZE = 256,
	LOG_MQTT_BUFFER_COUNT = 128,
};

struct statistics;
enum stat_frame_type;

struct log_via_mqtt_thread_context {
	struct mosquitto *mosq;        // MQTT client
	struct rte_ring *mqtt_ring;    // global ring buffer
	struct rte_mempool *mqtt_pool; // memory pool for log messages
};

void log_via_mqtt_stats(enum stat_frame_type frame_type, struct statistics *stats);

#endif /*LOGVIAMQTT*/

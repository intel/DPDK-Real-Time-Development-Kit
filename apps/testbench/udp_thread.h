/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _UDP_THREAD_H_
#define _UDP_THREAD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#include "stat.h"
#include "thread.h"

struct udp_thread_configuration {
	/* UDP configuration */
	lport_id_t udp_lport_id;
	enum stat_frame_type frame_type;
	const char *traffic_class;
	bool udp_ignore_rx_errors;
	uint64_t udp_burst_period_ns;
	size_t udp_num_frames_per_cycle;
	const char *udp_payload_pattern;
	size_t udp_payload_pattern_length;
	size_t udp_frame_length;
	int udp_thread_cpu;
	uint16_t udp_port;
	uint16_t udp_src_port;
	const char *udp_destination;
	const char *udp_source;
	const char *udp_mac_destination;
};

#endif /* _UDP_THREAD_H_ */

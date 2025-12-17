/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _TSN_THREAD_H_
#define _TSN_THREAD_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <linux/if_ether.h>

#include "lport.h"
#include "security.h"
#include "stat.h"
#include "thread.h"

struct tsn_thread_configuration {
	/* TSN configuration */
	lport_id_t id;
	enum stat_frame_type frame_type;
	const char *traffic_class;
	bool tsn_tx_enabled;
	bool tsn_rx_enabled;
	bool tsn_tx_time_enabled;
	uint64_t tsn_tx_time_offset_ns;
	size_t tsn_num_frames_per_cycle;
	const char *tsn_payload_pattern;
	size_t tsn_payload_pattern_length;
	size_t tsn_frame_length;
	enum security_mode tsn_security_mode;
	enum security_algorithm tsn_security_algorithm;
	char *tsn_security_key;
	size_t tsn_security_key_length;
	char *tsn_security_iv_prefix;
	size_t tsn_security_iv_prefix_length;
	uint32_t tsn_lport_id;
	int tsn_thread_cpu;
	unsigned char *tsn_destination;
	uint64_t tsn_duration;

	/* TSN low/high specific */
	int vlan_id;
	int vlan_pcp;
	uint16_t frame_id;
};

#endif /* _TSN_THREAD_H_ */

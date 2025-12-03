/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2025-2025 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_TSN_H
#define __INCLUDE_TSN_H

#include <stdint.h>

int tsn_set_vlan_qid(uint16_t vlan_id, uint16_t queue_id);
uint16_t tsn_get_vlan_qid(uint16_t vlan_id);

#define LLDP_FAKE_VLAN_ID     200
#define UDP_HIGH_FAKE_VLAN_ID 201
#define UDP_LOW_FAKE_VLAN_ID  202

#endif /* __INCLUDE_TSN_H */

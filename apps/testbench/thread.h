/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _THREAD_H_
#define _THREAD_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <linux/if_ether.h>
#ifdef WITH_MQTT
#include <mosquitto.h>
#endif

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_ethdev.h>

#include "lport.h"
#include "thread_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct security_context;

struct thread_context {
    const char *name;             /* Thread name */
    volatile int stop;            /* Done? */
    int lcore_id;                 /* Core ID */
    struct rte_mbuf **mbufs;      /* Array of mbufs */
    uint64_t rx_sequence_counter; /* Rx cycle counter */
    uint64_t tx_sequence_counter; /* Tx cycle counter */

    rte_atomic32_t received_frames; /* Amount of frames received within cycle */
    lport_gen_config_t cfg;         /* General configuration */

    /* Data flow related */
    struct thread_context *next;         /* Pointer to next traffic class */
    rte_atomic16_t data_cond_var;        /* Cond var to signal Tx thread */
    rte_atomic64_t num_frames_available; /* How many frames are ready to be sent? */
    bool is_first;                       /* Is this the first active traffic class? */
    uint32_t meta_data_offset;           /* Where is the MetaData in the frame? */

    /* Security related */
    struct security_context *tx_security_context; /* Tx context for Auth and Crypt */
    struct security_context *rx_security_context; /* Rx context for Auth and Crypt */
    unsigned char *payload_pattern;               /* Frame payload pattern used for AE */
    size_t payload_pattern_length;                /* Length of payload pattern */

    rte_atomic16_t rx_do_once;     /* Do RX once flag */
    rte_atomic16_t tx_do_once;     /* Do TX once flag */
    rte_atomic16_t tx_gen_do_once; /* Do TX gen once flag */

    thread_timer_t *timer;       /* Timer */
    uint64_t tx_cycles;          /* Cycles since last Tx */
    uint64_t rx_cycles;          /* Cycles since last Rx */
    uint64_t previous_rx_cycles; /* Cycles since last Rx */
    uint64_t previous_tx_cycles; /* Cycles since last Tx */

    /* Thread private data */
    void *private_data; /* Pointer to private data e.g, a structure */
};

#ifdef __cplusplus
}
#endif

#endif /* _THREAD_H_ */

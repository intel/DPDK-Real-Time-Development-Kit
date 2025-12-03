// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include "config.h"
#include "functions.h"
#include "packet_handler.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "net_def.h"
#include "security.h"
#include "stat.h"
#include "thread.h"
#include "utils.h"

extern bool force_quit;

#define DEFAULT_BURST_COUNT 128

static int
pkt_handler_threads_routine(void *data __rte_unused)
{
    lport_id_t id = app_config.pkt_handler_lport_id;
    uint16_t pid = lport2pid(id), qid = lport2qid(id);
	struct rte_mbuf *mbufs[DEFAULT_BURST_COUNT];
	uint16_t nb_pkts;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u for %s\n", "Packet-Handler",
            lport_format(id), rte_lcore_id(), lport_format(id));

    while (!force_quit) {
		nb_pkts = rte_eth_rx_burst(pid, qid, mbufs, DEFAULT_BURST_COUNT);
		if (nb_pkts) {
			rte_pktmbuf_free_bulk(mbufs, nb_pkts);
			fprintf(stderr, "%s: Received %'u packets on %u:%u\n", __func__, nb_pkts, pid, qid);
		}

	    rte_eth_tx_burst(pid, qid, NULL, 0);
    }

    return 0;
}

static void
pkt_handler_threads_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;
}

static int
pkt_handler_threads_create(struct thread_context *thread_context)
{
    int ret = 0;

    if (!app_config.pkt_handler_enabled)
        goto out;

    thread_context->mbufs = rte_calloc_socket("PHMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        fprintf(stderr, "Failed to allocate mbufs!\n");
        ret = -ENOMEM;
        goto err_exit;
    }

out:
    return 0;

err_exit:
    pkt_handler_threads_free(thread_context);
    return ret;
}

static int
pkt_handler_init(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.pkt_handler_thread_cpu;
    return 0;
}

static int
pkt_handler_launch(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    if (!app_config.pkt_handler_enabled)
        return 0;

    if (pkt_handler_threads_create(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "Packet Handler Lcore %u is already running\n",
                 thread_context->lcore_id);
    return rte_eal_remote_launch(pkt_handler_threads_routine, thread_context,
                                 thread_context->lcore_id);
}

static void
pkt_handler_deinit(void *arg __rte_unused)
{
    if (app_config.pkt_handler_enabled)
        pkt_handler_threads_free(arg);
}

FUNCTION_REGISTER(pkt_handler, PKT_HANDLER_IDX);

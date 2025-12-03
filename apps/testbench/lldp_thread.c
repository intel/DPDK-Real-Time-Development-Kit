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
#include "tsn.h"
#include "functions.h"
#include "lldp_thread.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "net_def.h"
#include "security.h"
#include "stat.h"
#include "thread.h"
#include "utils.h"

static void
lldp_build_frame_from_rx(struct thread_context *thread_context __rte_unused, struct rte_mbuf *mbuf)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    uint64_t tx_mirror;
    struct reference_meta_data *meta;

    rte_pktmbuf_data_len(mbuf) = app_config.lldp_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = app_config.lldp_frame_length;

    /* One task: set up source address. */
    memcpy(&eth->src_addr, lport_mac_address(app_config.lldp_lport_id), ETH_ALEN);

    /* One task: Set the tx timestamp. */
    meta      = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *, sizeof(*eth));
    tx_mirror = clock_gettime_ns();
    tx_timestamp_to_meta_data(meta, tx_mirror + (app_config.application_tx_base_offset_ns -
                                                 app_config.application_rx_base_offset_ns));
}

static void
lldp_initialize_frame(struct thread_context *thread_context __rte_unused, struct rte_mbuf *mbuf)
{
    struct reference_meta_data *meta;
    size_t payload_offset;
    struct rte_ether_hdr *eth;

    rte_pktmbuf_data_len(mbuf) = app_config.lldp_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = app_config.lldp_frame_length;

    memset(rte_pktmbuf_mtod(mbuf, char *), 0, app_config.lldp_frame_length);

    /*
     * LldpFrame:
     *   Destination (multicast)
     *   Source
     *   Ether type: 88cc
     *   Cycle counter
     *   Payload
     *   Padding to maxFrame
     */

    eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

    /* Ethernet header */
    memcpy(eth->dst_addr.addr_bytes, app_config.lldp_destination, ETH_ALEN);
    memcpy(eth->src_addr.addr_bytes, lport_mac_address(app_config.lldp_lport_id), ETH_ALEN);
    eth->ether_type = htons(RTE_ETHER_TYPE_LLDP);

    /* Payload: SequenceCounter + Data */
    meta = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *, sizeof(*eth));
    memset(meta, '\0', sizeof(*meta));
    payload_offset = sizeof(*eth) + sizeof(*meta);
    memcpy(rte_pktmbuf_mtod_offset(mbuf, unsigned char *, payload_offset),
           app_config.lldp_payload_pattern, app_config.lldp_payload_pattern_length);
}

static void
lldp_thread_setup(void *arg)
{
    struct thread_context *thread_context = arg;

    (void)thread_context;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n", "LLDP",
            lport_format(app_config.lldp_lport_id), rte_lcore_id());
}

static void
lldp_gen_and_send_frames(struct thread_context *thread_context, uint64_t num_frames)
{
    struct reference_meta_data *meta;
    struct ethhdr *eth;
    uint64_t sequence_counter = thread_context->tx_sequence_counter;
    lport_id_t id             = app_config.lldp_lport_id;
    uint16_t nb_pkts          = (uint16_t)num_frames;
    struct rte_mbuf *mbufs[MAX_PKT_BURST];
    uint64_t tx_time;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: lport_tx_get_bulk() failed\n", __func__);
        return;
    }

    tx_time = clock_gettime_ns();

    /* Adjust meta data */
    for (int i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m = mbufs[i];

        lldp_initialize_frame(thread_context, m);

        meta = (struct reference_meta_data *)(rte_pktmbuf_mtod(m, unsigned char *) + sizeof(*eth));
        sequence_counter_to_meta_data(meta, sequence_counter + i, nb_pkts);

        tx_timestamp_to_meta_data(meta, tx_time);

        stat_frame_sent(LLDP_FRAME_TYPE, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
lldp_tx_thread_routine(void *data, bool signaled)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->tx_do_once))
        thread_timer_set(thread_context, TX_TIMER, NSEC_PER_SEC);
    else {
        uint64_t num_frames;

        if (!signaled)
            return;
        num_frames = atomic64_exchange(&thread_context->num_frames_available, 0);

        /*
         * Send LldpFrames, two possibilities:
         *  a) Generate it, or
         *  b) Use received ones if mirror enabled
         */
        if (!is_mirror_mode())
            lldp_gen_and_send_frames(thread_context, num_frames);
        else
            send_frames_common(LLDP_FRAME_TYPE, app_config.lldp_lport_id,
                               thread_context->meta_data_offset, num_frames, clock_gettime_ns());

        /* Signal next Tx thread */
        if (thread_context->next)
            do_signal(&thread_context->next->data_cond_var);
    }
}

static void
lldp_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context = data;
    const unsigned char *expected_pattern = (const unsigned char *)app_config.lldp_payload_pattern;
    const size_t expected_pattern_length  = app_config.lldp_payload_pattern_length;
    const size_t num_frames_per_cycle     = app_config.lldp_num_frames_per_cycle;
    const bool ignore_rx_errors           = app_config.lldp_ignore_rx_errors;
    const size_t frame_length             = app_config.lldp_frame_length;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    unsigned char *frame_data;
    size_t len;
    struct reference_meta_data *meta;
    uint64_t sequence_counter;
    uint64_t tx_timestamp;
    struct rte_mbuf *dropped[LPORT_PKTMBUF_FREE_PENDING_SZ];
    uint16_t drop_count = 0;

    for (uint16_t i = 0; i < nb_mbufs; i++) {
        struct rte_mbuf *mbuf = mbufs[i];

        frame_data = rte_pktmbuf_mtod(mbuf, unsigned char *);
        len        = rte_pktmbuf_pkt_len(mbuf);

        /* Process received frame. */
        if (len != frame_length) {
            log_message(LOG_LEVEL_WARNING, "LldpRx: Frame with wrong length received!\n");
            goto drop;
        }

        /*
         * Check cycle counter and payload. The ether type is checked by the
         * attached BPF filter.
         */
        meta             = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *,
                                                   thread_context->meta_data_offset);
        sequence_counter = meta_data_to_sequence_counter(meta, num_frames_per_cycle);

        tx_timestamp = meta_data_to_tx_timestamp(meta);

        out_of_order      = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch  = memcmp(frame_data + sizeof(struct ethhdr) + sizeof(*meta),
                                   expected_pattern, expected_pattern_length)
                                ? true
                                : false;
        frame_id_mismatch = false;

        stat_frame_received(LLDP_FRAME_TYPE, sequence_counter, out_of_order, payload_mismatch,
                            frame_id_mismatch, tx_timestamp);

        if (out_of_order) {
            if (!ignore_rx_errors)
                log_message(LOG_LEVEL_WARNING,
                            "LldpRx: frame[%" PRIu64 "] SequenceCounter mismatch: %" PRIu64 "!\n",
                            sequence_counter, thread_context->rx_sequence_counter);
            // adjust to missing sequence counters
            thread_context->rx_sequence_counter = ++sequence_counter;
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            log_message(LOG_LEVEL_WARNING, "LldpRx: frame[%" PRIu64 "] Payload Pattern mismatch!\n",
                        sequence_counter);
            goto drop;
        }

        /* If mirror enabled, assemble and store the frame for Tx later. */
        if (!is_mirror_mode())
            goto drop;

        lldp_build_frame_from_rx(thread_context, mbuf);

        lport_tx_buffer_add(app_config.lldp_lport_id, mbuf);
        continue;
    drop:
        dropped[drop_count++] = mbuf;
        if (drop_count == LPORT_PKTMBUF_FREE_PENDING_SZ) {
            rte_pktmbuf_free_bulk(dropped, drop_count);
            drop_count = 0;
        }
    }
    if (drop_count)
        rte_pktmbuf_free_bulk(dropped, drop_count);
}

static void
lldp_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)app_config.lldp_num_frames_per_cycle;
        uint64_t timo =
            (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns);

        lport_process_pkts(thread_context, burst_sz, app_config.lldp_lport_id, lldp_rx_frame, timo);
    }
}

static void
lldp_tx_generation_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->tx_gen_do_once))
        thread_timer_set(thread_context, TXGEN_TIMER, app_config.lldp_burst_period_ns);
    else
        atomic64_set(&thread_context->num_frames_available, app_config.lldp_num_frames_per_cycle);
}

static int
lldp_threads_routine(void *data)
{
    struct thread_context *thread_context = data;

    lldp_thread_setup(data);

    if (thread_timer_alloc(thread_context, "LLDP", MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, "LLDP rx", NULL, lldp_rx_thread_routine, data,
                         curr_ns, app_config.application_rx_base_offset_ns))
        return -1;
    if (thread_timer_add(thread_context, TX_TIMER, "LLDP tx", &thread_context->data_cond_var,
                         lldp_tx_thread_routine, data, curr_ns,
                         app_config.application_tx_base_offset_ns))
        return -1;
    if (!is_mirror_mode()) {
        if (thread_timer_add(thread_context, TXGEN_TIMER, "LLDP tx gen", NULL,
                             lldp_tx_generation_thread_routine, data, curr_ns,
                             app_config.lldp_burst_period_ns))
            return -1;
    }

    return thread_timer_run(thread_context);
}

static void
lldp_threads_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;
}

static int
lldp_threads_create(struct thread_context *thread_context)
{
    lport_id_t id = app_config.lldp_lport_id;

    if (!CONFIG_IS_TRAFFIC_CLASS_ACTIVE(lldp))
        goto out;

    thread_context->meta_data_offset = get_meta_data_offset(LLDP_FRAME_TYPE, SECURITY_MODE_NONE);

    thread_context->mbufs = rte_calloc_socket("lldpMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbufs on lport %s!\n", lport_format(id));
        goto err_exit;
    }
    if (tsn_set_vlan_qid(LLDP_FAKE_VLAN_ID, lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        goto err_exit;
    }

out:
    return 0;

err_exit:
    lldp_threads_free(thread_context);
    return -1;
}

static int
lldp_init(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.lldp_thread_cpu;
    return 0;
}

static int
lldp_launch(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    if (!app_config.lldp_enabled)
        return 0;

    if (lldp_threads_create(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "LLDP Lcore %u is already running\n", thread_context->lcore_id);
    return rte_eal_remote_launch(lldp_threads_routine, thread_context, thread_context->lcore_id);
}

static void
lldp_deinit(void *arg __rte_unused)
{
    if (app_config.lldp_enabled)
        lldp_threads_free(arg);
}

FUNCTION_REGISTER(lldp, LLDP_IDX);

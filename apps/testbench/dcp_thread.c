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
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/if_vlan.h>

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "config.h"
#include "tsn.h"
#include "dcp_thread.h"
#include "functions.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "security.h"
#include "stat.h"
#include "thread.h"
#include "thread_timer.h"
#include "utils.h"

static void
dcp_initialize_frame(struct thread_context *thread_context __rte_unused, struct rte_mbuf *mbuf)
{
    rte_pktmbuf_data_len(mbuf) = app_config.dcp_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = app_config.dcp_frame_length;

    memset(rte_pktmbuf_mtod(mbuf, char *), 0, app_config.dcp_frame_length);

    initialize_profinet_frame(
        SECURITY_MODE_NONE, rte_pktmbuf_mtod(mbuf, unsigned char *), rte_pktmbuf_data_len(mbuf),
        lport_mac_address(app_config.dcp_lport_id), app_config.dcp_destination,
        app_config.dcp_payload_pattern, app_config.dcp_payload_pattern_length,
        app_config.dcp_vid | app_config.dcp_pcp << VLAN_PCP_SHIFT, DCP_FRAMEID);
}

static void
dcp_gen_and_send_frames(struct thread_context *thread_context, uint64_t num_frames)
{
    struct vlan_ethernet_header *eth;
    struct profinet_rt_header *rt;
    uint64_t sequence_counter = thread_context->tx_sequence_counter;
    lport_id_t id             = app_config.dcp_lport_id;
    uint16_t nb_pkts          = (uint16_t)num_frames;
    struct rte_mbuf **mbufs   = thread_context->mbufs;
    uint64_t tx_time;

    if (num_frames == 0)
        return;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: lport_tx_get_bulk() failed\n", __func__);
        return;
    }

    tx_time = clock_gettime_ns();

    /* Adjust meta data */
    for (int i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m = mbufs[i];

        dcp_initialize_frame(thread_context, m);

        rt = (struct profinet_rt_header *)(rte_pktmbuf_mtod(m, unsigned char *) + sizeof(*eth));

        sequence_counter_to_meta_data(&rt->meta_data, sequence_counter + i, nb_pkts);

        tx_timestamp_to_meta_data(&rt->meta_data, tx_time);

        stat_frame_sent(DCP_FRAME_TYPE, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
dcp_thread_setup(void *arg)
{
    struct thread_context *thread_context = arg;

    (void)thread_context;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n", "DCP",
            lport_format(app_config.dcp_lport_id), rte_lcore_id());
}

static void
dcp_tx_thread_routine(void *data, bool signaled)
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
         * Send DcpFrames, two possibilities:
         *  a) Generate it, or
         *  b) Use received ones if mirror enabled
         */
        if (!is_mirror_mode())
            dcp_gen_and_send_frames(thread_context, num_frames);
        else
            send_frames_common(DCP_FRAME_TYPE, app_config.dcp_lport_id,
                               thread_context->meta_data_offset, num_frames, clock_gettime_ns());

        /* Signal next Tx thread */
        if (thread_context->next)
            do_signal(&thread_context->next->data_cond_var);
    }
}

static void
dcp_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context = data;
    const unsigned char *expected_pattern = (const unsigned char *)app_config.dcp_payload_pattern;
    const size_t expected_pattern_length  = app_config.dcp_payload_pattern_length;
    const size_t num_frames_per_cycle     = app_config.dcp_num_frames_per_cycle;
    size_t expected_frame_length          = app_config.dcp_frame_length;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    struct profinet_rt_header *rt;
    unsigned char *frame_data;
    size_t len;
    struct ethhdr *eth;
    void *p;
    uint16_t proto;
    uint64_t sequence_counter;
    uint64_t tx_timestamp;
    struct rte_mbuf *dropped[LPORT_PKTMBUF_FREE_PENDING_SZ];
    uint16_t drop_count = 0;

    for (uint16_t i = 0; i < nb_mbufs; i++) {
        struct rte_mbuf *mbuf = mbufs[i];

        frame_data = rte_pktmbuf_mtod(mbuf, unsigned char *);
        len        = rte_pktmbuf_pkt_len(mbuf);
        p          = frame_data;

        eth = p;
        if (eth->h_proto == htons(ETH_P_8021Q)) {
            struct vlan_ethernet_header *veth = p;

            proto = veth->vlan_encapsulated_proto;
            p     = PTR_ADD(p, sizeof(*veth));
        } else {
            proto = eth->h_proto;
            p     = PTR_ADD(p, sizeof(*eth));
            expected_frame_length -= sizeof(struct vlan_header);
        }

        if (proto != htons(ETH_P_PROFINET_RT)) {
            log_message(LOG_LEVEL_WARNING, "DcpRx: Not a Profinet frame received!\n");
            goto drop;
        }

        if (len != expected_frame_length) {
            log_message(LOG_LEVEL_WARNING, "DcpRx: Frame with wrong length %ld != %ld received!\n",
                        len, expected_frame_length);
            goto drop;
        }

        /*
         * Check cycle counter and payload. The frame id range is checked by the attached BPF
         * filter.
         */
        rt               = p;
        sequence_counter = meta_data_to_sequence_counter(&rt->meta_data, num_frames_per_cycle);

        tx_timestamp = meta_data_to_tx_timestamp(&rt->meta_data);

        out_of_order = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch =
            memcmp(PTR_ADD(p, sizeof(*rt)), expected_pattern, expected_pattern_length) ? true
                                                                                       : false;
        frame_id_mismatch = false;

        stat_frame_received(DCP_FRAME_TYPE, sequence_counter, out_of_order, payload_mismatch,
                            frame_id_mismatch, tx_timestamp);

        if (out_of_order) {
            log_message(LOG_LEVEL_WARNING,
                        "DcpRx: frame[%" PRIu64 "] SequenceCounter mismatch: %" PRIu64 "!\n",
                        sequence_counter, thread_context->rx_sequence_counter);
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            char buff[128] = {0};

            memcpy(buff, PTR_ADD(p, sizeof(*rt)), expected_pattern_length);
            log_message(LOG_LEVEL_WARNING,
                        "DcpRx: frame[%" PRIu64 "] Payload Pattern mismatch %s!\n",
                        sequence_counter, buff);
            goto drop;
        }

        /* If mirror enabled, assemble and store the frame for Tx later. */
        if (!is_mirror_mode())
            goto drop;

        /* Swap mac addresses inline */
        swap_mac_addresses(frame_data, len);

        lport_tx_buffer_add(app_config.dcp_lport_id, mbuf);
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
dcp_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)app_config.dcp_num_frames_per_cycle;
        uint64_t timo =
            (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns);

        lport_process_pkts(thread_context, burst_sz, app_config.dcp_lport_id, dcp_rx_frame, timo);
    }
}

static void
dcp_tx_generation_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->tx_gen_do_once))
        thread_timer_set(thread_context, TXGEN_TIMER, app_config.dcp_burst_period_ns);
    else
        atomic64_set(&thread_context->num_frames_available, app_config.dcp_num_frames_per_cycle);
}

static int
dcp_threads_routine(void *data)
{
    struct thread_context *thread_context = data;

    dcp_thread_setup(data);

    if (thread_timer_alloc(thread_context, "DCP", MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, "DCP-Rx", NULL, dcp_rx_thread_routine, data,
                         curr_ns, app_config.application_rx_base_offset_ns))
        return -1;

    if (thread_timer_add(thread_context, TX_TIMER, "DCP-Tx", &thread_context->data_cond_var,
                         dcp_tx_thread_routine, data, curr_ns,
                         app_config.application_tx_base_offset_ns))
        return -1;
    if (!is_mirror_mode()) {
        if (thread_timer_add(thread_context, TXGEN_TIMER, "DCP TXGEN", NULL,
                             dcp_tx_generation_thread_routine, data, curr_ns,
                             app_config.dcp_burst_period_ns))
            return -1;
    }

    return thread_timer_run(thread_context);
}

static void
dcp_thread_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;
}

static int
dcp_threads_create(struct thread_context *thread_context)
{
    if (!CONFIG_IS_TRAFFIC_CLASS_ACTIVE(dcp))
        return 0;

    thread_context->meta_data_offset = get_meta_data_offset(DCP_FRAME_TYPE, SECURITY_MODE_NONE);

    thread_context->mbufs = rte_calloc_socket("dcpMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbufs!\n");
        return -ENOMEM;
    }
    lport_id_t id = app_config.dcp_lport_id;
    if (tsn_set_vlan_qid(app_config.dcp_vid, lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        return -EINVAL;
    }
    return 0;
}

static int
dcp_init(void *arg)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.dcp_thread_cpu;
    return 0;
}

static int
dcp_launch(void *arg)
{
    struct thread_context *thread_context = arg;

    if (!app_config.dcp_enabled)
        return 0;

    if (dcp_threads_create(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "DCP Lcore %u has a running thread\n", thread_context->lcore_id);

    return rte_eal_remote_launch(dcp_threads_routine, thread_context, thread_context->lcore_id);
}

static void
dcp_deinit(void *arg)
{
    if (app_config.dcp_enabled)
        dcp_thread_free(arg);
}

FUNCTION_REGISTER(dcp, DCP_IDX);

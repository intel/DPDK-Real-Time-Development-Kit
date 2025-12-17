// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2022-2024 Linutronix GmbH
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
#include <linux/if_vlan.h>

#include <sys/socket.h>

#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include "config.h"
#include "tsn.h"
#include "functions.h"
#include "layer2_thread.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "security.h"
#include "stat.h"
#include "thread.h"
#include "thread_timer.h"
#include "tx_time.h"
#include "utils.h"

static void
l2_initialize_frame(struct thread_context *thread_context __rte_unused, struct rte_mbuf *mbuf)
{
    struct vlan_ethernet_header *eth;
    struct reference_meta_data *meta;
    size_t payload_offset;

    rte_pktmbuf_data_len(mbuf) = app_config.l2_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = app_config.l2_frame_length;

    memset(rte_pktmbuf_mtod(mbuf, char *), 0, app_config.l2_frame_length);

    /*
     * L2Frame:
     *   Destination
     *   Source
     *   VLAN tag
     *   Ether type
     *   Cycle counter
     *   Payload
     *   Padding to maxFrame
     */

    eth = rte_pktmbuf_mtod(mbuf, struct vlan_ethernet_header *);
    meta  = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *, sizeof(*eth));

    /* Ethernet header */
    memcpy(eth->destination, app_config.l2_destination, ETH_ALEN);
    memcpy(eth->source, lport_mac_address(app_config.l2_lport_id), ETH_ALEN);

    /* VLAN Header */
    eth->vlan_proto              = htons(RTE_ETHER_TYPE_VLAN);
    eth->vlantci                 = htons(app_config.l2_vid | app_config.l2_pcp << VLAN_PCP_SHIFT);
    eth->vlan_encapsulated_proto = htons(app_config.l2_ether_type);

    /* Generic L2 header */
    meta->frame_counter = 0;
    meta->cycle_counter = 0;

    /* Payload */
    payload_offset = sizeof(*eth) + sizeof(*meta);
    memcpy(rte_pktmbuf_mtod_offset(mbuf, unsigned char *, payload_offset),
           app_config.l2_payload_pattern, app_config.l2_payload_pattern_length);

    /* Padding: '\0' */
}

static void
l2_gen_and_send_frames(struct thread_context *thread_context)
{
    struct vlan_ethernet_header *eth;
    struct reference_meta_data *meta;
    uint64_t sequence_counter = thread_context->tx_sequence_counter;
    lport_id_t id             = app_config.l2_lport_id;
    uint16_t nb_pkts          = (uint16_t)app_config.l2_num_frames_per_cycle;
    struct rte_mbuf **mbufs   = thread_context->mbufs;
    uint64_t tx_time;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: rte_mempool_get_bulk() failed for %u mbufs\n", __func__,
                    nb_pkts);
        return;
    }

    tx_time = clock_gettime_ns();

    /* Adjust meta data */
    for (uint16_t i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m = mbufs[i];

        l2_initialize_frame(thread_context, m);

        meta = rte_pktmbuf_mtod_offset(m, struct reference_meta_data *, sizeof(*eth));
        sequence_counter_to_meta_data(meta, sequence_counter + i, nb_pkts);

        tx_timestamp_to_meta_data(meta, tx_time);

        stat_frame_sent(L2_FRAME_TYPE, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
l2_thread_setup(void *arg)
{
    struct thread_context *thread_context = arg;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n", "L2",
            lport_format(app_config.l2_lport_id), rte_lcore_id());

    thread_context->payload_pattern +=
        sizeof(struct vlan_ethernet_header) + sizeof(struct profinet_secure_header);
    thread_context->payload_pattern_length =
        app_config.l2_frame_length - sizeof(struct vlan_ethernet_header) -
        sizeof(struct profinet_secure_header) - sizeof(struct security_checksum);
}

static void
l2_tx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->tx_do_once))
        thread_timer_set(thread_context, TX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        if (!is_mirror_mode())
            l2_gen_and_send_frames(thread_context);
        else
            send_frames_common(L2_FRAME_TYPE, app_config.l2_lport_id,
                               thread_context->meta_data_offset, app_config.l2_num_frames_per_cycle,
                               clock_gettime_ns());
    }
}

static void
l2_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context = data;
    const unsigned char *expected_pattern = (const unsigned char *)app_config.l2_payload_pattern;
    const size_t expected_pattern_length  = app_config.l2_payload_pattern_length;
    const size_t num_frames_per_cycle     = app_config.l2_num_frames_per_cycle;
    const bool ignore_rx_errors           = app_config.l2_ignore_rx_errors;
    size_t expected_frame_length          = app_config.l2_frame_length;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    uint64_t tx_mirror;
    void *frame_data;
    size_t len;
    uint64_t sequence_counter;
    uint64_t tx_timestamp;
    bool vlan_tag_missing;
    struct rte_ether_hdr *eth;
    uint16_t ether_type;
    struct rte_mbuf *dropped[LPORT_PKTMBUF_FREE_PENDING_SZ];
    uint16_t drop_count = 0;
    struct reference_meta_data *meta;

    for (uint16_t i = 0; i < nb_mbufs; i++) {
        struct rte_mbuf *mbuf = mbufs[i];

        frame_data = rte_pktmbuf_mtod(mbuf, void *);
        len        = rte_pktmbuf_pkt_len(mbuf);
        eth        = (struct rte_ether_hdr *)frame_data;

        if (len < sizeof(struct vlan_ethernet_header)) {
            log_message(LOG_LEVEL_WARNING, "L2Rx: Too small frame received!\n");
            goto drop;
        }

        if (eth->ether_type == htons(ETH_P_8021Q)) {
            struct vlan_ethernet_header *veth = frame_data;

            ether_type       = htons(veth->vlan_encapsulated_proto);
            meta             = PTR_ADD(frame_data, sizeof(*veth));
            vlan_tag_missing = false;
        } else {
            ether_type = htons(eth->ether_type);
            meta       = PTR_ADD(frame_data, sizeof(*eth));
            expected_frame_length -= sizeof(struct vlan_header);
            vlan_tag_missing = true;
        }

        if (ether_type != app_config.l2_ether_type) {
            log_message(LOG_LEVEL_WARNING, "L2Rx: Frame with wrong Ether Type received %04x!\n",
                        ether_type);
            goto drop;
        }

        /* Check frame length: VLAN tag might be stripped or not. Check it. */
        if (len != expected_frame_length) {
            log_message(LOG_LEVEL_WARNING, "L2Rx: Frame with wrong length received %'ld != %'ld!\n",
                        len, expected_frame_length);
            goto drop;
        }

        sequence_counter = meta_data_to_sequence_counter(meta, num_frames_per_cycle);

        tx_timestamp = meta_data_to_tx_timestamp(meta);
        tx_mirror    = clock_gettime_ns();
        tx_timestamp_to_meta_data(meta, tx_mirror + (app_config.application_tx_base_offset_ns -
                                                     app_config.application_rx_base_offset_ns));

        out_of_order      = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch  = memcmp(&meta[1], expected_pattern, expected_pattern_length) ? true : false;
        frame_id_mismatch = false;

        stat_frame_received(L2_FRAME_TYPE, sequence_counter, out_of_order, payload_mismatch,
                            frame_id_mismatch, tx_timestamp);

        if (out_of_order) {
            if (!ignore_rx_errors)
                log_message(LOG_LEVEL_WARNING,
                            "L2Rx: frame[%" PRIu64 "] SequenceCounter mismatch: %" PRIu64 "!\n",
                            sequence_counter, thread_context->rx_sequence_counter);
            // adjust to missing sequence counters
            thread_context->rx_sequence_counter = ++sequence_counter;
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            log_message(LOG_LEVEL_WARNING, "L2Rx: frame[%" PRIu64 "] Payload Pattern mismatch!\n",
                        sequence_counter);
            goto drop;
        }

        /*
         * If mirror enabled, assemble and store the frame for Tx later.
         *
         * In case of XDP the Rx umem area will be reused for Tx.
         */
        if (!is_mirror_mode())
            goto drop;

        /* Re-add vlan tag */
        if (vlan_tag_missing)
            insert_vlan_tag(frame_data, len,
                            app_config.l2_vid | app_config.l2_pcp << VLAN_PCP_SHIFT);

        /* Swap mac addresses inline */
        swap_mac_addresses(frame_data, len);

        lport_tx_buffer_add(app_config.l2_lport_id, mbuf);
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
l2_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)app_config.dcp_num_frames_per_cycle;
        uint64_t timo =
            (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns);

        lport_process_pkts(thread_context, burst_sz, app_config.l2_lport_id, l2_rx_frame, timo);
    }
}

static int
l2_threads_routine(void *data)
{
    struct thread_context *thread_context = data;

	fprintf(stderr, "L2 thread running on lcore %u\n", rte_lcore_id());
    l2_thread_setup(data);

    if (thread_timer_alloc(thread_context, "L2", MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, "L2-Rx", NULL, l2_rx_thread_routine, data,
                         curr_ns, app_config.application_rx_base_offset_ns))
        return -1;
    if (thread_timer_add(thread_context, TX_TIMER, "L2-Tx", NULL, l2_tx_thread_routine, data,
                         curr_ns, app_config.application_tx_base_offset_ns))
        return -1;

    return thread_timer_run(thread_context);
}

static int
l2_threads_create(struct thread_context *thread_context)
{
    if (!CONFIG_IS_TRAFFIC_CLASS_ACTIVE(l2))
        goto out;

    thread_context->meta_data_offset = get_meta_data_offset(L2_FRAME_TYPE, SECURITY_MODE_NONE);

    lport_id_t id         = app_config.l2_lport_id;
    thread_context->mbufs = rte_calloc_socket("l2Mbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbufs for lport %s!\n", lport_format(id));
        goto err_exit;
    }
    if (tsn_set_vlan_qid(app_config.l2_vid, lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        goto err_exit;
    }
out:
    return 0;
err_exit:
    return -1;
}

static void
l2_threads_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;
}

static int
l2_init(void *arg)
{
    struct thread_context *thread_context = (struct thread_context *)arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.l2_thread_cpu;
    return 0;
}

static int
l2_launch(void *arg)
{
    struct thread_context *thread_context = (struct thread_context *)arg;

    if (!app_config.l2_enabled)
        return 0;

    if (l2_threads_create(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING) {
        rte_exit(EXIT_FAILURE, "L2 Lcore %u is already running\n", thread_context->lcore_id);
	}

    return rte_eal_remote_launch(l2_threads_routine, thread_context, thread_context->lcore_id);
}

static void
l2_deinit(void *arg)
{
    struct thread_context *thread_context = (struct thread_context *)arg;

    if (app_config.l2_enabled)
        l2_threads_free(thread_context);
}

FUNCTION_REGISTER(l2, L2_IDX);

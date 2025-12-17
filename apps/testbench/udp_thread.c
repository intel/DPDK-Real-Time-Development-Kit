// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_vlan.h>

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_udp.h>

#include "config.h"
#include "tsn.h"
#include "functions.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "net_def.h"
#include "stat.h"
#include "thread.h"
#include "thread_timer.h"
#include "udp_thread.h"
#include "utils.h"

static void
udp_initialize_frame(struct thread_context *thread_context, struct rte_mbuf *mbuf)
{
    struct udp_thread_configuration *udp_config = thread_context->private_data;
    struct reference_meta_data *meta;
    struct rte_ether_hdr *eth;
    struct rte_ipv4_hdr *ip;
    struct rte_udp_hdr *udp;
    unsigned char *src_mac = lport_mac_address(udp_config->udp_lport_id);

    rte_pktmbuf_data_len(mbuf) = udp_config->udp_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = udp_config->udp_frame_length;

	memset(rte_pktmbuf_mtod(mbuf, char *), 0, udp_config->udp_frame_length);

    /*
     * UdpFrame:
     *   Cycle counter
     *   Payload
     *   Padding to maxFrame
     */

    /* src and dest addr */
    eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    ip  = rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, sizeof(*eth));
    udp = rte_pktmbuf_mtod_offset(mbuf, struct rte_udp_hdr *, (sizeof(*eth) + sizeof(*ip)));

    memcpy(&eth->src_addr, src_mac, sizeof(eth->src_addr));
    memcpy(&eth->dst_addr, udp_config->udp_mac_destination, sizeof(eth->src_addr));
    eth->ether_type = htons(ETH_P_IP);

    /* IP header */
    ip->version_ihl  = 0x45;
    ip->time_to_live = 64;
    ip->total_length = htons(udp_config->udp_frame_length - sizeof(*eth));
    inet_pton(AF_INET, (const char *)udp_config->udp_source, &ip->src_addr);
    inet_pton(AF_INET, udp_config->udp_destination, &ip->dst_addr);
    ip->next_proto_id = IPPROTO_UDP;
    ip->packet_id     = 0;
    ip->hdr_checksum  = 0;
    ip->hdr_checksum  = rte_ipv4_cksum(ip);

    /* UDP header */
    udp->src_port    = htons(udp_config->udp_src_port);
    udp->dst_port    = htons(udp_config->udp_port);
    udp->dgram_len   = htons(udp_config->udp_frame_length - sizeof(*eth) - sizeof(*ip));
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
    if (udp->dgram_cksum == 0)
        udp->dgram_cksum = 0xFFFF;

    /* Payload: SequenceCounter + Data */
    meta = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *,
                                   (sizeof(*eth) + sizeof(*ip) + sizeof(*udp)));
    memset(meta, '\0', sizeof(*meta));
    meta++;
    memcpy(meta, udp_config->udp_payload_pattern, udp_config->udp_payload_pattern_length);

    /* Padding: '\0' */
}

static void
udp_gen_and_send_frame(struct thread_context *thread_context,
                       const struct udp_thread_configuration *udp_config)
{
    struct reference_meta_data *meta;
    lport_id_t id    = udp_config->udp_lport_id;
    uint16_t nb_pkts = (uint16_t)udp_config->udp_num_frames_per_cycle;
    struct rte_mbuf *mbufs[MAX_PKT_BURST];
    uint64_t tx_time, sequence_counter = thread_context->tx_sequence_counter;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: lport_tx_get_bulk() failed\n", __func__);
        return;
    }

    tx_time = clock_gettime_ns();

    for (int i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m = mbufs[i];

        udp_initialize_frame(thread_context, m);

        /* Adjust meta data */
        meta = rte_pktmbuf_mtod_offset(m, struct reference_meta_data *,
                                       thread_context->meta_data_offset);
        sequence_counter_to_meta_data(meta, sequence_counter + i,
                                      udp_config->udp_num_frames_per_cycle);

        tx_timestamp_to_meta_data(meta, tx_time);

        stat_frame_sent(udp_config->frame_type, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
udp_thread_setup(void *arg)
{
    struct thread_context *thread_context              = arg;
    struct udp_thread_configuration *udp_thread_config = thread_context->private_data;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n",
            udp_thread_config->traffic_class, lport_format(udp_thread_config->udp_lport_id),
            rte_lcore_id());
}

static void
udp_tx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context       = data;
    struct udp_thread_configuration *udp_config = thread_context->private_data;
    const bool mirror_enabled                   = is_mirror_mode();
    uint64_t num_frames;

    if (do_once(&thread_context->tx_do_once))
        thread_timer_set(thread_context, TX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        if (signaled) {
            num_frames = atomic64_exchange(&thread_context->num_frames_available, 0);

            /*
             * Send UdpFrames, two possibilities:
             *  a) Generate it, or
             *  b) Use received ones if mirror enabled
             */
            if (mirror_enabled)
                send_frames_common(udp_config->frame_type, udp_config->udp_lport_id,
                                   thread_context->meta_data_offset,
                                   udp_config->udp_num_frames_per_cycle, clock_gettime_ns());
            else {
                if (num_frames)
                    udp_gen_and_send_frame(thread_context, udp_config);
            }

            /* Signal next Tx thread */
            if (thread_context->next)
                do_signal(&thread_context->next->data_cond_var);
        }
    }
}

static void
udp_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context       = data;
    struct udp_thread_configuration *udp_config = thread_context->private_data;
    const unsigned char *expected_pattern = (const unsigned char *)udp_config->udp_payload_pattern;
    const size_t expected_pattern_length  = udp_config->udp_payload_pattern_length;
    const size_t num_frames_per_cycle     = udp_config->udp_num_frames_per_cycle;
    const size_t frame_length             = udp_config->udp_frame_length;
    unsigned char *frame_data;
    size_t len;
    uint64_t tx_mirror, tx_timestamp;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    struct reference_meta_data *meta;
    uint64_t sequence_counter;
    struct rte_mbuf *dropped[LPORT_PKTMBUF_FREE_PENDING_SZ];
    uint16_t drop_count = 0;

    for (uint16_t i = 0; i < nb_mbufs; i++) {
        struct rte_mbuf *mbuf = mbufs[i];

        frame_data = rte_pktmbuf_mtod(mbuf, unsigned char *);
        len        = rte_pktmbuf_pkt_len(mbuf);

        /* Process received packet. */
        if (len != frame_length) {
            log_message(LOG_LEVEL_WARNING, "%sRx: Frame with wrong length received!\n",
                        udp_config->traffic_class);
            goto drop;
        }

        tx_mirror = clock_gettime_ns();

        /*
         * Check cycle counter and payload. The ether type is checked by the
         * attached BPF filter.
         */
        meta                = rte_pktmbuf_mtod_offset(mbuf, struct reference_meta_data *,
                                                      (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                                        sizeof(struct rte_udp_hdr)));
        sequence_counter = meta_data_to_sequence_counter(meta, num_frames_per_cycle);

        tx_timestamp = meta_data_to_tx_timestamp(meta);

        tx_timestamp_to_meta_data(meta, tx_mirror + (app_config.application_tx_base_offset_ns -
                                                     app_config.application_rx_base_offset_ns));

        out_of_order      = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch  = memcmp((meta + 1), expected_pattern, expected_pattern_length) ? true : false;
        frame_id_mismatch = false;

        stat_frame_received(udp_config->frame_type, sequence_counter, out_of_order,
                            payload_mismatch, frame_id_mismatch, tx_timestamp);

        if (out_of_order) {
            log_message(LOG_LEVEL_WARNING,
                        "%sRx: frame[%" PRIu64 "] SequenceCounter mismatch: %" PRIu64 "!\n",
                        udp_config->traffic_class, sequence_counter,
                        thread_context->rx_sequence_counter);
            // adjust to missing sequence counters
            thread_context->rx_sequence_counter = ++sequence_counter;
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            log_message(LOG_LEVEL_WARNING,
                        "%sRx: frame[%" PRIu64 "] Payload Pattern mismatch!\n",
                        udp_config->traffic_class, sequence_counter);
            goto drop;
        }

        if (!is_mirror_mode())
            goto drop;

        /* Swap mac addresses inline */
        swap_mac_addresses(frame_data, len);

        lport_tx_buffer_add(udp_config->udp_lport_id, mbuf);
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
udp_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context       = data;
    struct udp_thread_configuration *udp_config = thread_context->private_data;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)udp_config->udp_num_frames_per_cycle;
        uint64_t timo =
            (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns);

        lport_process_pkts(thread_context, burst_sz, udp_config->udp_lport_id, udp_rx_frame, timo);
    }
}

static void
udp_tx_generation_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context             = data;
    const struct udp_thread_configuration *udp_config = thread_context->private_data;

    if (do_once(&thread_context->tx_gen_do_once))
        thread_timer_set(thread_context, TXGEN_TIMER, udp_config->udp_burst_period_ns);
    else
        atomic64_set(&thread_context->num_frames_available, udp_config->udp_num_frames_per_cycle);
}

static void
udp_threads_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;

    rte_free((void *)thread_context->private_data);

    thread_context->private_data = NULL;
}

static int
udp_threads_create(struct thread_context *thread_context,
                   struct udp_thread_configuration *udp_config)
{
    thread_context->private_data = udp_config;

    if (udp_config->frame_type == UDP_HIGH_FRAME_TYPE && !CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_high))
        goto out;
    if (udp_config->frame_type == UDP_LOW_FRAME_TYPE && !CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_low))
        goto out;

    thread_context->meta_data_offset =
        get_meta_data_offset(udp_config->frame_type, SECURITY_MODE_NONE);

    lport_id_t id         = udp_config->udp_lport_id;
    thread_context->mbufs = rte_calloc_socket("udpMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbufs for lport %s\n", lport_format(id));
        goto err_exit;
    }

    if (tsn_set_vlan_qid((udp_config->frame_type == UDP_HIGH_FRAME_TYPE) ? UDP_HIGH_FAKE_VLAN_ID
                                                                         : UDP_LOW_FAKE_VLAN_ID,
                         lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        goto err_exit;
    }

out:
    return 0;
err_exit:
    return -1;
}

static int
udp_threads_routine(void *data)
{
    struct thread_context *thread_context              = data;
    struct udp_thread_configuration *udp_thread_config = thread_context->private_data;
    const char *name = udp_thread_config->frame_type == UDP_HIGH_FRAME_TYPE ? "UDP High"
                                                                            : "UDP Low";

    udp_thread_setup(data);

    if (thread_timer_alloc(thread_context, name, MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, "UDP rx", NULL, udp_rx_thread_routine, data,
                         curr_ns, app_config.application_rx_base_offset_ns))
        return -1;
    if (thread_timer_add(thread_context, TX_TIMER, "UDP tx", &thread_context->data_cond_var,
                         udp_tx_thread_routine, data, curr_ns,
                         app_config.application_tx_base_offset_ns))
        return -1;
    if (!is_mirror_mode()) {
        if (thread_timer_add(thread_context, TXGEN_TIMER, "UDP tx gen", NULL,
                             udp_tx_generation_thread_routine, data, curr_ns,
                             app_config.udp_high_burst_period_ns))
            return -1;
    }

    return thread_timer_run(thread_context);
}

static int
udp_low_threads_init(struct thread_context *udp_thread_context)
{
    struct udp_thread_configuration *udp_config;

    udp_config =
        rte_calloc_socket("udpLow", 1, sizeof(*udp_config), RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!udp_config)
        return -ENOMEM;

    udp_config->udp_lport_id               = app_config.udp_low_lport_id;
    udp_config->frame_type                 = UDP_LOW_FRAME_TYPE;
    udp_config->traffic_class              = stat_frame_type_to_string(UDP_LOW_FRAME_TYPE);
    udp_config->udp_burst_period_ns        = app_config.udp_low_burst_period_ns;
    udp_config->udp_num_frames_per_cycle   = app_config.udp_low_num_frames_per_cycle;
    udp_config->udp_payload_pattern        = app_config.udp_low_payload_pattern;
    udp_config->udp_payload_pattern_length = app_config.udp_low_payload_pattern_length;
    udp_config->udp_frame_length           = app_config.udp_low_frame_length;
    udp_config->udp_thread_cpu             = app_config.udp_low_thread_cpu;
    udp_config->udp_port                   = app_config.udp_low_port;
    udp_config->udp_src_port               = app_config.udp_low_src_port;
    udp_config->udp_destination            = app_config.udp_low_destination;
    udp_config->udp_source                 = app_config.udp_low_source;
    udp_config->udp_mac_destination        = app_config.udp_low_mac_destination;

    return udp_threads_create(udp_thread_context, udp_config);
}

static int
udp_low_init(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.udp_low_thread_cpu;
    return 0;
}

static int
udp_low_launch(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    if (!app_config.udp_low_enabled)
        return 0;
    if (udp_low_threads_init(thread_context))
        return -1;
    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "UDP Low Lcore %u is already running\n", thread_context->lcore_id);
    return rte_eal_remote_launch(udp_threads_routine, thread_context, thread_context->lcore_id);
}

static void
udp_low_deinit(void *arg __rte_unused)
{
    if (app_config.udp_low_enabled)
        udp_threads_free(arg);
}

FUNCTION_REGISTER(udp_low, UDP_LOW_IDX);

static int
udp_high_threads_init(struct thread_context *udp_thread_context)
{
    struct udp_thread_configuration *udp_config;

    udp_config =
        rte_calloc_socket("udpHigh", 1, sizeof(*udp_config), RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!udp_config)
        return -ENOMEM;

    udp_config->udp_lport_id               = app_config.udp_high_lport_id;
    udp_config->frame_type                 = UDP_HIGH_FRAME_TYPE;
    udp_config->traffic_class              = stat_frame_type_to_string(UDP_HIGH_FRAME_TYPE);
    udp_config->udp_burst_period_ns        = app_config.udp_high_burst_period_ns;
    udp_config->udp_num_frames_per_cycle   = app_config.udp_high_num_frames_per_cycle;
    udp_config->udp_payload_pattern        = app_config.udp_high_payload_pattern;
    udp_config->udp_payload_pattern_length = app_config.udp_high_payload_pattern_length;
    udp_config->udp_frame_length           = app_config.udp_high_frame_length;
    udp_config->udp_thread_cpu             = app_config.udp_high_thread_cpu;
    udp_config->udp_port                   = app_config.udp_high_port;
    udp_config->udp_src_port               = app_config.udp_high_src_port;
    udp_config->udp_destination            = app_config.udp_high_destination;
    udp_config->udp_source                 = app_config.udp_high_source;
    udp_config->udp_mac_destination        = app_config.udp_high_mac_destination;

    return udp_threads_create(udp_thread_context, udp_config);
}

static int
udp_high_init(void *arg)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.udp_high_thread_cpu;
    return 0;
}

static int
udp_high_launch(void *arg)
{
    struct thread_context *thread_context = arg;

    if (!app_config.udp_high_enabled)
        return 0;
    if (udp_high_threads_init(thread_context))
        return -1;
    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "UDP High Lcore %u is already running\n", thread_context->lcore_id);
    return rte_eal_remote_launch(udp_threads_routine, thread_context, thread_context->lcore_id);
}

static void
udp_high_deinit(void *arg __rte_unused)
{
    if (app_config.udp_high_enabled)
        udp_threads_free(arg);
}

FUNCTION_REGISTER(udp_high, UDP_HIGH_IDX);

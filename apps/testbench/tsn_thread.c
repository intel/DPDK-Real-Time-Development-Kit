// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2021-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
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

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include "config.h"
#include "tsn.h"
#include "functions.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "net_def.h"
#include "security.h"
#include "stat.h"
#include "thread.h"
#include "thread_timer.h"
#include "tsn_thread.h"
#include "utils.h"

static void
tsn_initialize_frame(struct thread_context *thread_context, struct rte_mbuf *mbuf)
{
    struct tsn_thread_configuration *tsn_config = thread_context->private_data;

    rte_pktmbuf_data_len(mbuf) = tsn_config->tsn_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = tsn_config->tsn_frame_length;

    memset(rte_pktmbuf_mtod(mbuf, char *), 0, tsn_config->tsn_frame_length);

    initialize_profinet_frame(
        tsn_config->tsn_security_mode, rte_pktmbuf_mtod(mbuf, unsigned char *),
        rte_pktmbuf_data_len(mbuf), lport_mac_address(tsn_config->tsn_lport_id),
        tsn_config->tsn_destination, tsn_config->tsn_payload_pattern,
        tsn_config->tsn_payload_pattern_length,
        tsn_config->vlan_id | tsn_config->vlan_pcp << VLAN_PCP_SHIFT, tsn_config->frame_id);
}

static void
tsn_gen_and_send_frames(struct thread_context *thread_context, uint64_t num_frames)
{
    struct tsn_thread_configuration *tsn_config = thread_context->private_data;
    lport_id_t id                               = tsn_config->tsn_lport_id;
    uint16_t nb_pkts                            = (uint16_t)num_frames;
    struct rte_mbuf **mbufs                     = thread_context->mbufs;
    uint64_t sequence_counter                   = thread_context->tx_sequence_counter;
    uint64_t tx_time;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_WARNING, "%s: lport_tx_get_bulk() failed\n", __func__);
        return;
    }

    tx_time = clock_gettime_ns();

    for (int i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m                       = mbufs[i];
        struct prepare_frame_config frame_config = {0};

        tsn_initialize_frame(thread_context, m);

        frame_config.mode             = tsn_config->tsn_security_mode;
        frame_config.security_context = thread_context->tx_security_context;
        frame_config.iv_prefix        = (const unsigned char *)tsn_config->tsn_security_iv_prefix;
        frame_config.payload_pattern  = thread_context->payload_pattern;
        frame_config.payload_pattern_length = thread_context->payload_pattern_length;
        frame_config.frame_data             = rte_pktmbuf_mtod(m, unsigned char *);
        frame_config.frame_length           = rte_pktmbuf_data_len(m);
        frame_config.num_frames_per_cycle   = nb_pkts;
        frame_config.sequence_counter       = sequence_counter + i;
        frame_config.tx_timestamp           = tx_time;
        frame_config.meta_data_offset       = thread_context->meta_data_offset;

        int err = prepare_frame_for_tx(&frame_config);
        if (err)
            log_message(LOG_LEVEL_ERROR, "%sTx: Failed to prepare frame for Tx!\n",
                        tsn_config->traffic_class);

        stat_frame_sent(tsn_config->frame_type, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
tsn_thread_setup(void *arg)
{
    struct thread_context *thread_context       = arg;
    struct tsn_thread_configuration *tsn_config = thread_context->private_data;

    prepare_openssl(thread_context->tx_security_context);
    prepare_openssl(thread_context->rx_security_context);

    thread_context->payload_pattern +=
        sizeof(struct vlan_ethernet_header) + sizeof(struct profinet_secure_header);
    thread_context->payload_pattern_length =
        tsn_config->tsn_frame_length - sizeof(struct vlan_ethernet_header) -
        sizeof(struct profinet_secure_header) - sizeof(struct security_checksum);
}

static void
tsn_tx_thread_routine(void *data, bool signaled)
{
    struct thread_context *thread_context       = data;
    struct tsn_thread_configuration *tsn_config = thread_context->private_data;

    if (do_once(&thread_context->tx_do_once))
        thread_timer_set(thread_context, TX_TIMER,
                         !thread_context->is_first ? NSEC_PER_SEC
                                                   : app_config.application_base_cycle_time_ns);
    else {
        uint64_t num_frames;

        if (!thread_context->is_first) {
            if (!signaled)
                return;
        }
        num_frames = tsn_config->tsn_num_frames_per_cycle;
        /*
         * Send TsnFrames, two possibilities:
         *  a) Generate it, or
         *  b) Use received ones if mirror enabled
         */
        if (!is_mirror_mode())
            tsn_gen_and_send_frames(thread_context, num_frames);
        else
            send_frames_common(tsn_config->frame_type, tsn_config->tsn_lport_id,
                               thread_context->meta_data_offset, num_frames, clock_gettime_ns());

        /* Signal next Tx thread */
        if (thread_context->next)
            do_signal(&thread_context->next->data_cond_var);
    }
}

static void
tsn_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context             = data;
    const struct tsn_thread_configuration *tsn_config = thread_context->private_data;
    const unsigned char *expected_pattern = (const unsigned char *)tsn_config->tsn_payload_pattern;
    struct security_context *security_context = thread_context->rx_security_context;
    const size_t expected_pattern_length      = tsn_config->tsn_payload_pattern_length;
    size_t expected_frame_length              = tsn_config->tsn_frame_length;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    unsigned char plaintext[MAX_FRAME_SIZE];
    uint64_t tx_mirror;
    struct profinet_secure_header *srt;
    struct profinet_rt_header *rt;
    uint64_t sequence_counter;
    uint64_t tx_timestamp;
    bool vlan_tag_missing;
    unsigned char *frame_data;
    size_t len;
    void *p;
    struct ethhdr *eth;
    uint16_t frame_id;
    uint16_t proto;
    struct rte_mbuf *dropped[LPORT_PKTMBUF_FREE_PENDING_SZ];
    uint16_t drop_count = 0;

    for (uint16_t i = 0; i < nb_mbufs; i++) {
        struct rte_mbuf *mbuf = mbufs[i];

        frame_data = rte_pktmbuf_mtod(mbuf, unsigned char *);
        len        = rte_pktmbuf_pkt_len(mbuf);
        p          = frame_data;

        if (len < sizeof(struct vlan_ethernet_header)) {
            log_message(LOG_LEVEL_WARNING, "%sRx: Too small frame received!\n",
                        tsn_config->traffic_class);
            goto drop;
        }
        eth = p;
        if (eth->h_proto == htons(ETH_P_8021Q)) {
            struct vlan_ethernet_header *veth = p;

            proto            = veth->vlan_encapsulated_proto;
            p                = PTR_ADD(p, sizeof(*veth));
            vlan_tag_missing = false;
        } else {
            proto = eth->h_proto;
            p     = PTR_ADD(p, sizeof(*eth));
            expected_frame_length -= sizeof(struct vlan_header);
            vlan_tag_missing = true;
        }

        if (proto != htons(ETH_P_PROFINET_RT)) {
            log_message(LOG_LEVEL_WARNING, "%sRx: Not a Profinet frame received!\n",
                        tsn_config->traffic_class);
            goto drop;
        }

        /* Check frame length: VLAN tag might be stripped or not. Check it. */
        if (len != expected_frame_length) {
            log_message(LOG_LEVEL_WARNING, "%sRx: Frame with wrong length received!\n",
                        tsn_config->traffic_class);
            goto drop;
        }

        tx_mirror = clock_gettime_ns();

        /* Check cycle counter, frame id range and payload. */
        if (tsn_config->tsn_security_mode == SECURITY_MODE_NONE) {
            rt = p;
            p  = PTR_ADD(p, sizeof(*rt));

            frame_id = be16toh(rt->frame_id);
            sequence_counter =
                meta_data_to_sequence_counter(&rt->meta_data, tsn_config->tsn_num_frames_per_cycle);

            tx_timestamp = meta_data_to_tx_timestamp(&rt->meta_data);
            tx_timestamp_to_meta_data(&rt->meta_data,
                                      tx_mirror + (app_config.application_tx_base_offset_ns -
                                                   app_config.application_rx_base_offset_ns));
        } else if (tsn_config->tsn_security_mode == SECURITY_MODE_AO) {
            unsigned char *begin_of_security_checksum;
            unsigned char *begin_of_aad_data;
            size_t size_of_eth_header;
            size_t size_of_aad_data;
            struct security_iv iv;
            int ret;

            srt = p;
            p   = PTR_ADD(p, sizeof(*srt));

            frame_id         = be16toh(srt->frame_id);
            sequence_counter = meta_data_to_sequence_counter(&srt->meta_data,
                                                             tsn_config->tsn_num_frames_per_cycle);

            tx_timestamp = meta_data_to_tx_timestamp(&srt->meta_data);

            /* Authenticate received Profinet Frame */
            size_of_eth_header = vlan_tag_missing ? sizeof(struct ethhdr)
                                                  : sizeof(struct vlan_ethernet_header);

            begin_of_aad_data = frame_data + size_of_eth_header;
            size_of_aad_data  = len - size_of_eth_header - sizeof(struct security_checksum);
            begin_of_security_checksum = frame_data + (len - sizeof(struct security_checksum));

            prepare_iv((const unsigned char *)tsn_config->tsn_security_iv_prefix, sequence_counter,
                       &iv);

            ret = security_decrypt(security_context, NULL, 0, begin_of_aad_data, size_of_aad_data,
                                   begin_of_security_checksum, (unsigned char *)&iv, NULL);
            if (ret)
                log_message(LOG_LEVEL_WARNING, "%sRx: frame[%" PRIu64 "] Not authenticated\n",
                            tsn_config->traffic_class, sequence_counter);

            tx_timestamp_to_meta_data(&srt->meta_data,
                                      tx_mirror + (app_config.application_tx_base_offset_ns -
                                                   app_config.application_rx_base_offset_ns));
            security_encrypt(security_context, NULL, 0, begin_of_aad_data, size_of_aad_data,
                             (unsigned char *)&iv, NULL, begin_of_security_checksum);

        } else {
            unsigned char *begin_of_security_checksum;
            unsigned char *begin_of_ciphertext;
            unsigned char *begin_of_aad_data;
            size_t size_of_ciphertext;
            size_t size_of_eth_header;
            size_t size_of_aad_data;
            struct security_iv iv;
            int ret;

            srt = p;

            frame_id         = be16toh(srt->frame_id);
            sequence_counter = meta_data_to_sequence_counter(&srt->meta_data,
                                                             tsn_config->tsn_num_frames_per_cycle);

            tx_timestamp = meta_data_to_tx_timestamp(&srt->meta_data);

            /* Authenticate received Profinet Frame */
            size_of_eth_header = vlan_tag_missing ? sizeof(struct ethhdr)
                                                  : sizeof(struct vlan_ethernet_header);

            begin_of_aad_data          = frame_data + size_of_eth_header;
            size_of_aad_data           = sizeof(*srt);
            begin_of_security_checksum = frame_data + (len - sizeof(struct security_checksum));
            begin_of_ciphertext        = frame_data + size_of_eth_header + sizeof(*srt);
            size_of_ciphertext         = len - sizeof(struct vlan_ethernet_header) -
                                 sizeof(struct profinet_secure_header) -
                                 sizeof(struct security_checksum);

            prepare_iv((const unsigned char *)tsn_config->tsn_security_iv_prefix, sequence_counter,
                       &iv);

            ret = security_decrypt(security_context, begin_of_ciphertext, size_of_ciphertext,
                                   begin_of_aad_data, size_of_aad_data, begin_of_security_checksum,
                                   (unsigned char *)&iv, plaintext);
            if (ret)
                log_message(LOG_LEVEL_WARNING,
                            "%sRx: frame[%" PRIu64 "] Not authenticated and decrypted\n",
                            tsn_config->traffic_class, sequence_counter);

            /* plaintext points to the decrypted payload */
            p = plaintext;

            tx_timestamp_to_meta_data(&srt->meta_data,
                                      tx_mirror + (app_config.application_tx_base_offset_ns -
                                                   app_config.application_rx_base_offset_ns));

            security_encrypt(security_context, thread_context->payload_pattern,
                             thread_context->payload_pattern_length, begin_of_aad_data,
                             size_of_aad_data, (unsigned char *)&iv, begin_of_ciphertext,
                             begin_of_security_checksum);
        }

        out_of_order      = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch  = memcmp(p, expected_pattern, expected_pattern_length) ? true : false;
        frame_id_mismatch = frame_id != tsn_config->frame_id ? true : false;

        stat_frame_received(tsn_config->frame_type, sequence_counter, out_of_order,
                            payload_mismatch, frame_id_mismatch, tx_timestamp);

        if (frame_id_mismatch) {
            log_message(LOG_LEVEL_WARNING,
                        "%sRx: Frame[%llu] FrameId expected mismatch: %u != %u!\n",
                        tsn_config->traffic_class, (unsigned long long)sequence_counter,
                        (unsigned)tsn_config->frame_id, (unsigned)frame_id);
            goto drop;
        }
        if (out_of_order) {
            log_message(LOG_LEVEL_WARNING,
                        "%sRx: Frame[%llu] SequenceCounter expected mismatch: %llu!\n",
                        tsn_config->traffic_class, (unsigned long long)sequence_counter,
                        (unsigned long long)thread_context->rx_sequence_counter);
            // adjust to missing sequence counters
            thread_context->rx_sequence_counter = ++sequence_counter;
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            log_message(LOG_LEVEL_WARNING, "%sRx: frame[%llu] Payload Pattern mismatch!\n",
                        tsn_config->traffic_class, (unsigned long long)sequence_counter);
            goto drop;
        }

        // If mirror enabled, assemble and store the frame for Tx later.
        if (!is_mirror_mode())
            goto drop;

        /* Re-add vlan tag */
        if (vlan_tag_missing)
            insert_vlan_tag(frame_data, len,
                            tsn_config->vlan_id | tsn_config->vlan_pcp << VLAN_PCP_SHIFT);

        /* Swap mac addresses inline */
        swap_mac_addresses(frame_data, len);

        lport_tx_buffer_add(tsn_config->tsn_lport_id, mbuf);
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
tsn_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context             = data;
    const struct tsn_thread_configuration *tsn_config = thread_context->private_data;
    uint64_t timo =
        (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns) / 4;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)tsn_config->tsn_num_frames_per_cycle;

        lport_process_pkts(thread_context, burst_sz, tsn_config->tsn_lport_id, tsn_rx_frame, timo);
    }
}

static void
tsn_threads_free(struct thread_context *thread_context)
{
    const struct tsn_thread_configuration *tsn_config;

    if (!thread_context)
        return;

    tsn_config = thread_context->private_data;

    security_exit(thread_context->tx_security_context);
    security_exit(thread_context->rx_security_context);

    rte_free((void *)(uintptr_t)tsn_config);

    thread_context->private_data        = NULL;
    thread_context->payload_pattern     = NULL;
    thread_context->tx_security_context = NULL;
    thread_context->rx_security_context = NULL;
}

static int
tsn_threads_init(struct thread_context *thread_context, struct tsn_thread_configuration *tsn_config)
{
    lport_id_t id = tsn_config->tsn_lport_id;
    int ret;

    if (tsn_config->frame_type == TSN_HIGH_FRAME_TYPE &&
        !CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_high)) {
        ret = 0;
        goto out;
    }
    if (tsn_config->frame_type == TSN_LOW_FRAME_TYPE && !CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_low)) {
        ret = 0;
        goto out;
    }

    thread_context->private_data = tsn_config;

    if (tsn_config->tsn_security_mode != SECURITY_MODE_NONE) {
        thread_context->tx_security_context = security_init(
            tsn_config->tsn_security_algorithm, (unsigned char *)tsn_config->tsn_security_key);
        if (!thread_context->tx_security_context) {
            fprintf(stderr, "Failed to initialize Tx security context!\n");
            ret = -ENOMEM;
            goto err_exit;
        }

        thread_context->rx_security_context = security_init(
            tsn_config->tsn_security_algorithm, (unsigned char *)tsn_config->tsn_security_key);
        if (!thread_context->rx_security_context) {
            fprintf(stderr, "Failed to initialize Rx security context!\n");
            ret = -ENOMEM;
            goto err_exit;
        }
    } else {
        thread_context->tx_security_context = NULL;
        thread_context->rx_security_context = NULL;
    }
    thread_context->meta_data_offset =
        get_meta_data_offset(tsn_config->frame_type, tsn_config->tsn_security_mode);

    thread_context->mbufs = rte_calloc_socket("tsnMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        fprintf(stderr, "Failed to allocate mbufs!\n");
        ret = -ENOMEM;
        goto err_exit;
    }
    if (tsn_set_vlan_qid(tsn_config->vlan_id, lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        goto err_exit;
    }
out:
    return 0;

err_exit:
    tsn_threads_free(thread_context);
    return ret;
}

static int
tsn_threads_routine(void *data)
{
    struct thread_context *thread_context       = data;
    struct tsn_thread_configuration *tsn_config = thread_context->private_data;
    lport_id_t id                               = tsn_config->tsn_lport_id;
    bool tsn_low                                = (tsn_config->frame_type == TSN_LOW_FRAME_TYPE);

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n", tsn_config->traffic_class,
            lport_format(id), rte_lcore_id());

    tsn_thread_setup(data);

    if (thread_timer_alloc(thread_context, (tsn_low) ? "TSNLow" : "TSNHigh", MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, (tsn_low) ? "TSNLow-Rx" : "TSNHigh-Rx", NULL,
                         tsn_rx_thread_routine, data, curr_ns,
                         app_config.application_rx_base_offset_ns))
        return -1;

    if (thread_timer_add(thread_context, TX_TIMER, (tsn_low) ? "TSNLow-Tx" : "TSNHigh-Tx",
                         &thread_context->data_cond_var, tsn_tx_thread_routine, data, curr_ns,
                         app_config.application_tx_base_offset_ns))
        return -1;

    return thread_timer_run(thread_context);
}

static int
tsn_low_threads_init(struct thread_context *tsn_thread_context)
{
    struct tsn_thread_configuration *tsn_config;

    tsn_config =
        rte_calloc_socket("tsnLow", 1, sizeof(*tsn_config), RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!tsn_config)
        return -ENOMEM;

    tsn_config->tsn_lport_id                  = app_config.tsn_low_lport_id;
    tsn_config->frame_type                    = TSN_LOW_FRAME_TYPE;
    tsn_config->traffic_class                 = stat_frame_type_to_string(TSN_LOW_FRAME_TYPE);
    tsn_config->tsn_tx_time_enabled           = app_config.tsn_low_tx_time_enabled;
    tsn_config->tsn_tx_time_offset_ns         = app_config.tsn_low_tx_time_offset_ns;
    tsn_config->tsn_num_frames_per_cycle      = app_config.tsn_low_num_frames_per_cycle;
    tsn_config->tsn_payload_pattern           = app_config.tsn_low_payload_pattern;
    tsn_config->tsn_payload_pattern_length    = app_config.tsn_low_payload_pattern_length;
    tsn_config->tsn_frame_length              = app_config.tsn_low_frame_length;
    tsn_config->tsn_security_mode             = app_config.tsn_low_security_mode;
    tsn_config->tsn_security_algorithm        = app_config.tsn_low_security_algorithm;
    tsn_config->tsn_security_key              = app_config.tsn_low_security_key;
    tsn_config->tsn_security_key_length       = app_config.tsn_low_security_key_length;
    tsn_config->tsn_security_iv_prefix        = app_config.tsn_low_security_iv_prefix;
    tsn_config->tsn_security_iv_prefix_length = app_config.tsn_low_security_iv_prefix_length;
    tsn_config->tsn_lport_id                  = app_config.tsn_low_lport_id;
    tsn_config->tsn_thread_cpu                = app_config.tsn_low_thread_cpu;
    tsn_config->tsn_destination               = app_config.tsn_low_destination;
    tsn_config->vlan_id                       = app_config.tsn_low_vid;
    tsn_config->vlan_pcp                      = app_config.tsn_low_pcp;
    tsn_config->frame_id =
        tsn_config->tsn_security_mode == SECURITY_MODE_NONE ? TSN_LOW_FRAMEID : TSN_LOW_SEC_FRAMEID;

    return tsn_threads_init(tsn_thread_context, tsn_config);
}

static int
tsn_low_init(void *arg)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.tsn_low_thread_cpu;
    return 0;
}

static int
tsn_low_launch(void *arg)
{
    struct thread_context *thread_context = arg;

    if (!app_config.tsn_low_enabled)
        return 0;
    if (tsn_low_threads_init(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "TSN Low Lcore %u has a running thread\n", thread_context->lcore_id);
    return rte_eal_remote_launch(tsn_threads_routine, thread_context, thread_context->lcore_id);
}

static void
tsn_low_deinit(void *arg)
{
    if (app_config.tsn_low_enabled)
        tsn_threads_free(arg);
}

FUNCTION_REGISTER(tsn_low, TSN_LOW_IDX);

static int
tsn_high_threads_init(struct thread_context *tsn_thread_context)
{
    struct tsn_thread_configuration *tsn_config;

    tsn_config =
        rte_calloc_socket("tsnHight", 1, sizeof(*tsn_config), RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!tsn_config)
        return -ENOMEM;

    tsn_config->tsn_lport_id                  = app_config.tsn_high_lport_id;
    tsn_config->frame_type                    = TSN_HIGH_FRAME_TYPE;
    tsn_config->traffic_class                 = stat_frame_type_to_string(TSN_HIGH_FRAME_TYPE);
    tsn_config->tsn_tx_time_enabled           = app_config.tsn_high_tx_time_enabled;
    tsn_config->tsn_tx_time_offset_ns         = app_config.tsn_high_tx_time_offset_ns;
    tsn_config->tsn_num_frames_per_cycle      = app_config.tsn_high_num_frames_per_cycle;
    tsn_config->tsn_payload_pattern           = app_config.tsn_high_payload_pattern;
    tsn_config->tsn_payload_pattern_length    = app_config.tsn_high_payload_pattern_length;
    tsn_config->tsn_frame_length              = app_config.tsn_high_frame_length;
    tsn_config->tsn_security_mode             = app_config.tsn_high_security_mode;
    tsn_config->tsn_security_algorithm        = app_config.tsn_high_security_algorithm;
    tsn_config->tsn_security_key              = app_config.tsn_high_security_key;
    tsn_config->tsn_security_key_length       = app_config.tsn_high_security_key_length;
    tsn_config->tsn_security_iv_prefix        = app_config.tsn_high_security_iv_prefix;
    tsn_config->tsn_security_iv_prefix_length = app_config.tsn_high_security_iv_prefix_length;
    tsn_config->tsn_thread_cpu                = app_config.tsn_high_thread_cpu;
    tsn_config->tsn_destination               = app_config.tsn_high_destination;
    tsn_config->vlan_id                       = app_config.tsn_high_vid;
    tsn_config->vlan_pcp                      = app_config.tsn_high_pcp;
    tsn_config->frame_id                      = tsn_config->tsn_security_mode == SECURITY_MODE_NONE
                                                    ? TSN_HIGH_FRAMEID
                                                    : TSN_HIGH_SEC_FRAMEID;

    return tsn_threads_init(tsn_thread_context, tsn_config);
}

static int
tsn_high_init(void *arg)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.tsn_high_thread_cpu;
    return 0;
}

static int
tsn_high_launch(void *arg)
{
    struct thread_context *thread_context = arg;

    if (!app_config.tsn_high_enabled)
        return 0;
    if (tsn_high_threads_init(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "TSN High Lcore %u has a running thread\n",
                 thread_context->lcore_id);

    return rte_eal_remote_launch(tsn_threads_routine, thread_context, thread_context->lcore_id);
}

static void
tsn_high_deinit(void *arg)
{
    if (app_config.tsn_high_enabled)
        tsn_threads_free(arg);
}

FUNCTION_REGISTER(tsn_high, TSN_HIGH_IDX);

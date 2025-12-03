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
#include <linux/if_vlan.h>

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include "config.h"
#include "tsn.h"
#include "functions.h"
#include "log.h"
#include "lport.h"
#include "lport-priv.h"
#include "net_def.h"
#include "rta_thread.h"
#include "security.h"
#include "stat.h"
#include "utils.h"

static void
rta_initialize_frame(struct thread_context *thread_context __rte_unused, struct rte_mbuf *mbuf)
{
    uint16_t frame_id = app_config.rta_security_mode == SECURITY_MODE_NONE ? RTA_FRAMEID
                                                                           : RTA_SEC_FRAMEID;

    rte_pktmbuf_data_len(mbuf) = app_config.rta_frame_length;
    rte_pktmbuf_pkt_len(mbuf)  = app_config.rta_frame_length;

    memset(rte_pktmbuf_mtod(mbuf, char *), 0, app_config.rta_frame_length);

    initialize_profinet_frame(app_config.rta_security_mode, rte_pktmbuf_mtod(mbuf, unsigned char *),
                              rte_pktmbuf_data_len(mbuf),
                              lport_mac_address(app_config.rta_lport_id),
                              app_config.rta_destination, app_config.rta_payload_pattern,
                              app_config.rta_payload_pattern_length,
                              app_config.rta_vid | app_config.rta_pcp << VLAN_PCP_SHIFT, frame_id);
}

static void
rta_gen_and_send_frames(struct thread_context *thread_context, uint64_t num_frames)
{
    uint64_t sequence_counter = thread_context->tx_sequence_counter;
    lport_id_t id             = app_config.rta_lport_id;
    uint16_t nb_pkts          = (uint16_t)num_frames;
    struct rte_mbuf *mbufs[MAX_PKT_BURST];
    uint64_t tx_time;

	if (num_frames == 0)
		return;

    // Allocate pre-built mbufs using the mempool routine.
    if (lport_tx_get_bulk(id, mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: lport_tx_get_bulk() failed\n", __func__);
        return;
    }

    tx_time = clock_gettime_ns();

    for (int i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m = mbufs[i];
        struct prepare_frame_config frame_config;
        int err;

        rta_initialize_frame(thread_context, m);

        frame_config.mode             = app_config.rta_security_mode;
        frame_config.security_context = thread_context->tx_security_context;
        frame_config.iv_prefix        = (const unsigned char *)app_config.rta_security_iv_prefix;
        frame_config.payload_pattern  = thread_context->payload_pattern;
        frame_config.payload_pattern_length = thread_context->payload_pattern_length;
        frame_config.frame_data             = rte_pktmbuf_mtod(m, unsigned char *);
        frame_config.frame_length           = rte_pktmbuf_data_len(m);
        frame_config.num_frames_per_cycle   = nb_pkts;
        frame_config.sequence_counter       = sequence_counter + i;
        frame_config.tx_timestamp           = tx_time;
        frame_config.meta_data_offset       = thread_context->meta_data_offset;

        err = prepare_frame_for_tx(&frame_config);
        if (err)
            log_message(LOG_LEVEL_ERROR, "RTTx: Failed to prepare frame for Tx!\n");

        stat_frame_sent(RTA_FRAME_TYPE, sequence_counter + i, tx_time);

        lport_tx_buffer_add(id, m);
    }
    thread_context->tx_sequence_counter += nb_pkts;

    lport_send(id);
}

static void
rta_thread_setup(void *arg)
{
    struct thread_context *thread_context = arg;

    fprintf(stderr, "Starting %-14s thread for lport %s on lcore %u\n", "RTA",
            lport_format(app_config.rta_lport_id), rte_lcore_id());

    prepare_openssl(thread_context->tx_security_context);
    prepare_openssl(thread_context->rx_security_context);

    thread_context->payload_pattern +=
        sizeof(struct vlan_ethernet_header) + sizeof(struct profinet_secure_header);
    thread_context->payload_pattern_length =
        app_config.rta_frame_length - sizeof(struct vlan_ethernet_header) -
        sizeof(struct profinet_secure_header) - sizeof(struct security_checksum);
}

static void
rta_tx_thread_routine(void *data, bool signaled)
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
         * Send RtaFrames, two possibilities:
         *  a) Generate it, or
         *  b) Use received ones if mirror enabled
         */
        if (!is_mirror_mode())
            rta_gen_and_send_frames(thread_context, num_frames);
        else
            send_frames_common(RTA_FRAME_TYPE, app_config.rta_lport_id,
                               thread_context->meta_data_offset, num_frames, clock_gettime_ns());

        /* Signal next Tx thread */
        if (thread_context->next)
            do_signal(&thread_context->next->data_cond_var);
    }
}

static void
rta_rx_frame(void *data, struct rte_mbuf **mbufs, uint16_t nb_mbufs)
{
    struct thread_context *thread_context = data;
    const unsigned char *expected_pattern = (const unsigned char *)app_config.rta_payload_pattern;
    struct security_context *security_context = thread_context->rx_security_context;
    const size_t expected_pattern_length      = app_config.rta_payload_pattern_length;
    const size_t num_frames_per_cycle         = app_config.rta_num_frames_per_cycle;
    const bool ignore_rx_errors               = app_config.rta_ignore_rx_errors;
    size_t expected_frame_length              = app_config.rta_frame_length;
    bool out_of_order, payload_mismatch, frame_id_mismatch;
    unsigned char plaintext[MAX_FRAME_SIZE];
    uint64_t tx_mirror;
    uint16_t expected_frame_id;
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
            log_message(LOG_LEVEL_WARNING, "RtaRx: Too small frame received!\n");
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
            log_message(LOG_LEVEL_WARNING, "RtaRx: Not a Profinet frame received!\n");
            goto drop;
        }

        /* Check frame length: VLAN tag might be stripped or not. Check it. */
        if (len != expected_frame_length) {
            log_message(LOG_LEVEL_WARNING, "RtaRx: Frame with wrong length %ld != %ld received!\n",
                        len, expected_frame_length);
            goto drop;
        }

        tx_mirror = clock_gettime_ns();

        /* Check cycle counter, frame id range and payload. */
        if (app_config.rta_security_mode == SECURITY_MODE_NONE) {
            rt = p;
            p  = PTR_ADD(p, sizeof(*rt));

            frame_id         = be16toh(rt->frame_id);
            sequence_counter = meta_data_to_sequence_counter(&rt->meta_data, num_frames_per_cycle);

            tx_timestamp = meta_data_to_tx_timestamp(&rt->meta_data);
            tx_timestamp_to_meta_data(&rt->meta_data,
                                      tx_mirror + (app_config.application_tx_base_offset_ns -
                                                   app_config.application_rx_base_offset_ns));

        } else if (app_config.rta_security_mode == SECURITY_MODE_AO) {
            unsigned char *begin_of_security_checksum;
            unsigned char *begin_of_aad_data;
            size_t size_of_eth_header;
            size_t size_of_aad_data;
            struct security_iv iv;
            int ret;

            srt = p;
            p   = PTR_ADD(p, sizeof(*srt));

            frame_id         = be16toh(srt->frame_id);
            sequence_counter = meta_data_to_sequence_counter(&srt->meta_data, num_frames_per_cycle);

            tx_timestamp = meta_data_to_tx_timestamp(&srt->meta_data);

            /* Authenticate received Profinet Frame */
            size_of_eth_header = vlan_tag_missing ? sizeof(struct ethhdr)
                                                  : sizeof(struct vlan_ethernet_header);

            begin_of_aad_data = frame_data + size_of_eth_header;
            size_of_aad_data  = len - size_of_eth_header - sizeof(struct security_checksum);
            begin_of_security_checksum = frame_data + (len - sizeof(struct security_checksum));

            prepare_iv((const unsigned char *)app_config.rta_security_iv_prefix, sequence_counter,
                       &iv);

            ret = security_decrypt(security_context, NULL, 0, begin_of_aad_data, size_of_aad_data,
                                   begin_of_security_checksum, (unsigned char *)&iv, NULL);
            if (ret)
                log_message(LOG_LEVEL_WARNING, "RtaRx: frame[%" PRIu64 "] Not authenticated\n",
                            sequence_counter);

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
            sequence_counter = meta_data_to_sequence_counter(&srt->meta_data, num_frames_per_cycle);

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

            prepare_iv((const unsigned char *)app_config.rta_security_iv_prefix, sequence_counter,
                       &iv);

            ret = security_decrypt(security_context, begin_of_ciphertext, size_of_ciphertext,
                                   begin_of_aad_data, size_of_aad_data, begin_of_security_checksum,
                                   (unsigned char *)&iv, plaintext);
            if (ret)
                log_message(LOG_LEVEL_WARNING,
                            "RtaRx: frame[%" PRIu64 "] Not authenticated and decrypted\n",
                            sequence_counter);

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

        expected_frame_id = app_config.rta_security_mode == SECURITY_MODE_NONE ? RTA_FRAMEID
                                                                               : RTA_SEC_FRAMEID;

        out_of_order      = sequence_counter != thread_context->rx_sequence_counter ? true : false;
        payload_mismatch  = memcmp(p, expected_pattern, expected_pattern_length) ? true : false;
        frame_id_mismatch = frame_id != expected_frame_id ? true : false;

        stat_frame_received(RTA_FRAME_TYPE, sequence_counter, out_of_order, payload_mismatch,
                            frame_id_mismatch, tx_timestamp);

        if (frame_id_mismatch) {
            log_message(LOG_LEVEL_WARNING, "RtaRx: frame[%" PRIu64 "] FrameId mismatch: 0x%4x!\n",
                        sequence_counter, expected_frame_id);
            goto drop;
        }

        if (out_of_order) {
            if (!ignore_rx_errors)
                log_message(LOG_LEVEL_WARNING,
                            "RtaRx: frame[%" PRIu64 "] SequenceCounter mismatch: %" PRIu64 "!\n",
                            sequence_counter, thread_context->rx_sequence_counter);
            // adjust to missing sequence counters
            thread_context->rx_sequence_counter = ++sequence_counter;
            goto drop;
        }
        thread_context->rx_sequence_counter++;

        if (payload_mismatch) {
            log_message(LOG_LEVEL_WARNING, "RtaRx: frame[%" PRIu64 "] Payload Pattern mismatch!\n",
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
                            app_config.rta_vid | app_config.rta_pcp << VLAN_PCP_SHIFT);

        /* Swap mac addresses inline */
        swap_mac_addresses(frame_data, len);

        lport_tx_buffer_add(app_config.rta_lport_id, mbuf);
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
rta_rx_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;
    uint64_t timo =
        (app_config.application_tx_base_offset_ns - app_config.application_rx_base_offset_ns) / 4;

    if (do_once(&thread_context->rx_do_once))
        thread_timer_set(thread_context, RX_TIMER, app_config.application_base_cycle_time_ns);
    else {
        int burst_sz = (int)app_config.rta_num_frames_per_cycle;

        lport_process_pkts(thread_context, burst_sz, app_config.rta_lport_id, rta_rx_frame, timo);
    }
}

static void
rta_tx_generation_thread_routine(void *data, bool signaled __rte_unused)
{
    struct thread_context *thread_context = data;

    if (do_once(&thread_context->tx_gen_do_once))
        thread_timer_set(thread_context, TXGEN_TIMER, app_config.rta_burst_period_ns);
    else
        atomic64_set(&thread_context->num_frames_available, app_config.rta_num_frames_per_cycle);
}

static int
rta_threads_routine(void *data)
{
    struct thread_context *thread_context = data;

    rta_thread_setup(data);

    if (thread_timer_alloc(thread_context, "RTA", MAX_TIMERS) < 0)
        return -1;

    uint64_t curr_ns = clock_gettime_ns();

    if (thread_timer_add(thread_context, RX_TIMER, "RTA-Rx", NULL, rta_rx_thread_routine, data,
                         curr_ns, app_config.application_rx_base_offset_ns))
        return -1;
    if (thread_timer_add(thread_context, TX_TIMER, "RTA-Tx", &thread_context->data_cond_var,
                         rta_tx_thread_routine, data, curr_ns,
                         app_config.application_tx_base_offset_ns))
        return -1;
    if (!is_mirror_mode()) {
        if (thread_timer_add(thread_context, TXGEN_TIMER, "RTA-TxGen", NULL,
                             rta_tx_generation_thread_routine, data, curr_ns,
                             app_config.rta_burst_period_ns))
            return -1;
    }
    return thread_timer_run(thread_context);
}

static void
rta_threads_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;

    security_exit(thread_context->tx_security_context);
    security_exit(thread_context->rx_security_context);

    thread_context->payload_pattern     = NULL;
    thread_context->tx_security_context = NULL;
    thread_context->rx_security_context = NULL;
}

static int
rta_threads_create(struct thread_context *thread_context)
{
    lport_id_t id = app_config.rta_lport_id;
    int ret       = 0;

    if (!CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rta))
        goto out;

    if (app_config.rta_security_mode != SECURITY_MODE_NONE) {
        thread_context->tx_security_context = security_init(
            app_config.rta_security_algorithm, (unsigned char *)app_config.rta_security_key);
        if (!thread_context->tx_security_context) {
            fprintf(stderr, "Failed to initialize Tx security context!\n");
            ret = -ENOMEM;
            goto err_exit;
        }

        thread_context->rx_security_context = security_init(
            app_config.rta_security_algorithm, (unsigned char *)app_config.rta_security_key);
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
        get_meta_data_offset(RTA_FRAME_TYPE, app_config.rta_security_mode);

    thread_context->mbufs = rte_calloc_socket("rtaMbufs", MAX_PKT_BURST, sizeof(struct rte_mbuf *),
                                              RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (!thread_context->mbufs) {
        fprintf(stderr, "Failed to allocate mbufs!\n");
        ret = -ENOMEM;
        goto err_exit;
    }
    if (tsn_set_vlan_qid(app_config.rta_vid, lport2qid(id)) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to set VLAN ID %d for lport %s\n", lport2qid(id),
                 lport_format(id));
        goto err_exit;
    }
out:
    return 0;

err_exit:
    rta_threads_free(thread_context);
    return ret;
}

static int
rta_init(void *arg __rte_unused)
{
    struct thread_context *thread_context = arg;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.rta_thread_cpu;
    return 0;
}

static int
rta_launch(void *arg)
{
    struct thread_context *thread_context = arg;

    if (!app_config.rta_enabled)
        return 0;
    if (rta_threads_create(thread_context))
        return -1;
    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "RTA Lcore %u is already running\n", thread_context->lcore_id);
    return rte_eal_remote_launch(rta_threads_routine, thread_context, thread_context->lcore_id);
}

static void
rta_deinit(void *arg __rte_unused)
{
    if (app_config.rta_enabled)
        rta_threads_free(arg);
}

FUNCTION_REGISTER(rta, RTA_IDX);

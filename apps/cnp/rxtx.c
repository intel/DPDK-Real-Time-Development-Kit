/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"
#include "probe.h"
#include <rte_hexdump.h>
#include <unistd.h>

#define TX_READ_TIMESTAMP_TIMO 20000ULL        // 20us timeout

static int
poll_tx_timestamp(uint16_t port_id, uint64_t *tx_timestamp)
{
    struct timespec timestamp = {0};
    uint64_t start_ns         = clock_get_ns(),
             end_ns           = start_ns + TX_READ_TIMESTAMP_TIMO;        // 20us timeout
    int ret;

    do {
        ret = rte_eth_timesync_read_tx_timestamp(port_id, &timestamp);
        if (ret == 0) {        // Success
            *tx_timestamp = ts_to_ns(&timestamp);
            break;
        } else if (ret == -ENOTSUP) {
            // Hardware doesn't support or flag wasn't set
            break;
        }
        printf("Waiting for TX timestamp...\n");
        rte_pause();
    } while (ret == -EAGAIN && clock_get_ns() < end_ns);

    return ret;
}

static inline int
tx_timestamping(lport_t *lport)
{
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth;

    if ((m = rte_pktmbuf_alloc(lport->tx_mp)) == NULL) {
        lport->other_stats.no_mbufs++;
        printf("No mbufs available for Tx on port %u\n", lport->pid);
        return -1;
    }

    rte_pktmbuf_pkt_len(m)  = pinfo->pkt_length;
    rte_pktmbuf_data_len(m) = pinfo->pkt_length;
    eth                     = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    rte_ether_addr_copy(&lport->src_mac, &eth->src_addr);
    rte_ether_addr_copy(&lport->dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_1588);

    // Set TX timestamp flag if enabled
    if (_btst(HW_TIMESTAMP))
        m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;

    if (_btst(SW_TIMESTAMP)) {
        probe_payload_t *payload =
            (probe_payload_t *)(rte_pktmbuf_mtod_offset(m, char *, sizeof(struct rte_ether_hdr)));

        memset(payload, 0, sizeof(probe_payload_t));
        payload->magic           = rte_cpu_to_be_16(THE_MAGIC);
        payload->sequence_number = rte_cpu_to_be_32(lport->tx_sequence++);
        payload->packet_type     = TYPE_PROBE_SEND;
        payload->T1              = rte_cpu_to_be_64(clock_get_ns());        // Example timestamp
    }

    if (rte_eth_tx_burst(lport->pid, lport->qid, &m, 1) == 0) {
        rte_pktmbuf_free(m);
        lport->other_stats.tx_frame_errors++;
        printf("Failed to send packet on port %u\n", lport->pid);
        return -1;
    }
    if (_btst(HW_TIMESTAMP))
        if (poll_tx_timestamp(lport->pid, &lport->tx_timestamp))
			printf("Failed to get TX timestamp on port %u\n", lport->pid);

    return 0;
}

static inline int
rx_timestamping(lport_t *lport)
{
    struct rte_mbuf **rx_mbufs = lport->rx_mbufs;
    struct rte_mbuf **tx_mbufs = lport->tx_mbufs;
    struct rte_ether_addr tmp;
    uint64_t T1, delta_ns;
    uint16_t nb_rx, nb_tx;

    if ((nb_rx = rte_eth_rx_burst(lport->pid, lport->qid, rx_mbufs, RX_BURST_SIZE)) > 0) {
        struct rte_ether_hdr *eth_hdr;
        probe_payload_t *payload;

		if (nb_rx > 1)
			lport->other_stats.many_rx++;

        nb_tx = 0;
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *pkt = rx_mbufs[i];

            lport->other_stats.total_pkts.rx++;
            if (rte_pktmbuf_pkt_len(pkt) < sizeof(struct rte_ether_hdr) + sizeof(probe_payload_t)) {
                lport->other_stats.rx_frame_errors++;
                printf("Frame too short\n");
                rte_pktmbuf_free(pkt);
                continue;
            }

            eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_1588)) {
                lport->other_stats.rx_unknown_frames++;
                rte_pktmbuf_free(pkt);
                continue;
            }

            if (_btst(SW_TIMESTAMP)) {
                payload = (probe_payload_t *)(rte_pktmbuf_mtod_offset(
                    pkt, char *, sizeof(struct rte_ether_hdr)));

                if (rte_be_to_cpu_16(payload->magic) != THE_MAGIC) {
                    lport->other_stats.rx_no_probe_frames++;
                    printf("Invalid magic: 0x%04x\n", rte_be_to_cpu_16(payload->magic));
                    rte_pktmbuf_free(pkt);
                    continue;
                }

                T1       = rte_be_to_cpu_64(payload->T1);
                delta_ns = clock_get_ns() - T1;

                // Update RTT statistics
                lport->other_stats.rtt.count++;
                lport->other_stats.rtt.sum_ns += delta_ns;
                if (delta_ns < lport->other_stats.rtt.min_ns)
                    lport->other_stats.rtt.min_ns = delta_ns;
                if (delta_ns > lport->other_stats.rtt.max_ns)
                    lport->other_stats.rtt.max_ns = delta_ns;
            }

            if (_btst(HW_TIMESTAMP)) {
                // Check only the first packet for timestamp data else use system clock
                if (pkt->ol_flags & pinfo->rx_timestamp_flag) {
                    uint64_t rx_timestamp =
                        *RTE_MBUF_DYNFIELD(pkt, pinfo->rx_timestamp_offset, uint64_t *);

                    if (lport->tx_timestamp != UINT64_MAX) {
                        if (rx_timestamp >= lport->tx_timestamp) {
                            delta_ns = rx_timestamp - lport->tx_timestamp;

                            // Update HW RTT statistics
                            lport->other_stats.hw_rtt.count++;
                            lport->other_stats.hw_rtt.sum_ns += delta_ns;
                            if (delta_ns < lport->other_stats.hw_rtt.min_ns)
                                lport->other_stats.hw_rtt.min_ns = delta_ns;
                            if (delta_ns > lport->other_stats.hw_rtt.max_ns)
                                lport->other_stats.hw_rtt.max_ns = delta_ns;
                        }
                    } else
                        printf("No valid TX timestamp to calculate HW RTT\n");
                }
            }

            // Swap MAC addresses
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr, &tmp);
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr,
                                &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr);
            rte_ether_addr_copy(&tmp, &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr);

            tx_mbufs[nb_tx++] = pkt;
        }

        // Client mode - free the Rx packets
        if (pinfo->client_mode)
            rte_pktmbuf_free_bulk(tx_mbufs, nb_tx);
        else        // Server mode - echo back
            send_packets(lport->pid, lport->qid, tx_mbufs, nb_tx);
        lport->other_stats.total_pkts.tx += nb_tx;
    }

    return 0;
}

int
rxtx_routine(void *arg __rte_unused)
{
    uint16_t pid            = pinfo->lport_idx++;
    lport_t *lport          = &pinfo->lports[pid];
    uint64_t tx_begin_ns    = 0;
    uint64_t curr_ns        = 0;
    timestamping_fn rx_func = rx_timestamping, tx_func = tx_timestamping;

    printf("Starting RX/TX on port %u on lcore %u\n", pid, rte_lcore_id());
    lport->pid = pid;
    lport->qid = 0;
    if (port_init(lport) < 0) {
        stop_running();
        rte_exit(EXIT_FAILURE, "Cannot init lport %u:%u\n", pid, lport->qid);
    }

    while (is_link_up(pid) == false) {
        usleep(250000);
        if (!is_running())
            goto leave;
    }

    lport->other_stats.rtt.min_ns = 1000000UL;
    lport->other_stats.rtt.max_ns = 0;
    lport->other_stats.rtt.count  = 0;
    lport->other_stats.rtt.sum_ns = 0;

    lport->other_stats.hw_rtt.min_ns = 1000000UL;
    lport->other_stats.hw_rtt.max_ns = 0;
    lport->other_stats.hw_rtt.count  = 0;
    lport->other_stats.hw_rtt.sum_ns = 0;

    curr_ns     = clock_get_ns();
    tx_begin_ns = curr_ns + pinfo->tx_interval_ns;        // Start after 1 interval

    /* Run until the application has stopped or been killed. */
    while (is_running()) {
        curr_ns = clock_get_ns();
        /* Wait until the next cycle time */
        if (curr_ns >= tx_begin_ns) {
            tx_begin_ns = curr_ns + pinfo->tx_interval_ns;

            if (pinfo->client_mode && tx_func(lport) < 0)
                rte_exit(EXIT_FAILURE, "failed to send packet on port %u", lport->pid);
        }

        if (rx_func(lport) < 0)
            stop_running();
    }
leave:
    sleep(1);        // Give some time for RX packets to be processed

    return 0;
}

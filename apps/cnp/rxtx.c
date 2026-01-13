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

#define TX_READ_TIMESTAMP_TIMO \
    100        // 100 * 10us timeout (increased for better NIC compatibility)

static int
poll_tx_timestamp(uint16_t port_id, uint64_t *tx_timestamp, stats_t *stats)
{
    struct timespec timestamp = {0};
    int timo                  = TX_READ_TIMESTAMP_TIMO;
    int ret;

    do {
        ret = rte_eth_timesync_read_tx_timestamp(port_id, &timestamp);
        if (ret == -ENOTSUP) {
            printf("Port %u: Read Timestamp not supported by hardware\n", port_id);
            // Hardware doesn't support timestamping
            if (_btst(DEBUG_MODE))
                printf("TX timestamp not supported by hardware\n");
            stats->tx_timestamp_errors++;
            break;
        } else if (ret == 0) {        // Success
            *tx_timestamp = ts_to_ns(&timestamp);
            if (_btst(DEBUG_MODE))
                printf("Got TX timespec: %'" PRIu64 " sec, %'" PRIu64 " ns, ret %d\n",
                       (uint64_t)timestamp.tv_sec, (uint64_t)timestamp.tv_nsec, ret);
            break;
        }
        rte_pause();
    } while (--timo);

    if (ret == -EAGAIN || timo == 0) {
        stats->tx_timestamp_timeouts++;
        if (_btst(DEBUG_MODE))
            printf("TX timestamp polling timeout after %d * 10us\n", TX_READ_TIMESTAMP_TIMO);
    }

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

    if (_btst(SW_TIMESTAMP)) {
        probe_payload_t *payload =
            (probe_payload_t *)(rte_pktmbuf_mtod_offset(m, char *, sizeof(struct rte_ether_hdr)));

        memset(payload, 0, sizeof(probe_payload_t));
        payload->magic           = rte_cpu_to_be_16(THE_MAGIC);
        payload->sequence_number = rte_cpu_to_be_32(lport->tx_sequence++);
        payload->packet_type     = TYPE_PROBE_SEND;
        payload->T1              = rte_cpu_to_be_64(clock_get_ns());        // Example timestamp
    }

    m->ol_flags = 0;

    // Set TX timestamp flag if enabled
    if (_btst(HW_LAUNCH_TIME)) {
        uint64_t port_ns = port_clock_get_ns(lport->pid);        // Get port clock for launch time
        uint64_t packet_interval_ns = NSEC_PER_SEC / 60;         // Example interval

        m->ol_flags |= lport->tx_timestamp_flag;
        m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;
        *RTE_MBUF_DYNFIELD(m, lport->tx_timestamp_offset, uint64_t *) =
            port_ns + packet_interval_ns;
    }

    if (_btst(HW_TX_TIMESTAMP)) {
        // Initialize TX timestamp to invalid value
        lport->tx_timestamp = UINT64_MAX;
    }

    send_packets(lport->pid, lport->qid, &m, 1);

    if (_btst(HW_TX_TIMESTAMP)) {
        int ret = poll_tx_timestamp(lport->pid, &lport->tx_timestamp, &lport->other_stats);
        if (ret != 0) {
            // Keep previous timestamp on failure
            if (_btst(DEBUG_MODE))
                printf("Failed to get TX timestamp on port %u: %s\n", lport->pid,
                       rte_strerror(-ret));
        }
    }

    return 0;
}

static inline int
rx_timestamping(lport_t *lport)
{
    struct rte_mbuf **rx_mbufs = lport->rx_mbufs;
    struct rte_mbuf **tx_mbufs = lport->tx_mbufs;
    struct rte_ether_addr tmp;
    uint64_t T1, delta_ns;
    uint16_t nb_rx = 0, nb_tx = 0;

    if ((nb_rx = rte_eth_rx_burst(lport->pid, lport->qid, rx_mbufs, RX_BURST_SIZE)) > 0) {
        struct rte_ether_hdr *eth_hdr;
        probe_payload_t *payload;

        if (nb_rx > 1)
            lport->other_stats.many_rx++;

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

            if (_btst(HW_RX_TIMESTAMP)) {
                // Check packet for RX timestamp flag
                if (pkt->ol_flags & RTE_MBUF_F_RX_IEEE1588_TMST) {
                    uint64_t rx_timestamp =
                        *RTE_MBUF_DYNFIELD(pkt, lport->rx_timestamp_offset, uint64_t *);

                    if (lport->prev_rx_timestamp == 0)
                        lport->prev_rx_timestamp = rx_timestamp;

                    // Validate RX timestamp is non-zero and reasonable
                    if (rx_timestamp == 0) {
                        lport->other_stats.rx_timestamp_errors++;
                        if (_btst(DEBUG_MODE))
                            printf("Warning: RX timestamp is zero\n");
                    } else {
                        delta_ns = (rx_timestamp - lport->prev_rx_timestamp);

                        // Update HW Rx RTT statistics
                        lport->other_stats.hw_rx_rtt.count++;
                        lport->other_stats.hw_rx_rtt.sum_ns += delta_ns;
                        if (delta_ns < lport->other_stats.hw_rx_rtt.min_ns)
                            lport->other_stats.hw_rx_rtt.min_ns = delta_ns;
                        if (delta_ns > lport->other_stats.hw_rx_rtt.max_ns)
                            lport->other_stats.hw_rx_rtt.max_ns = delta_ns;
                        lport->prev_rx_timestamp = rx_timestamp;
                    }
                } else
                    lport->other_stats.rx_no_timestamp++;
            }

            if (_btst(HW_TX_TIMESTAMP)) {
                if (lport->tx_timestamp != UINT64_MAX && lport->tx_timestamp != 0) {
                    // Calculate one-way RTT as half the delta between RX and TX
                    // timestamps
                    if (lport->tx_timestamp > lport->prev_tx_timestamp) {
                        delta_ns = (lport->prev_tx_timestamp - lport->tx_timestamp);

                        // Sanity check: RTT should be reasonable (< 1 second)
                        if (delta_ns < NSEC_PER_SEC) {
                            // Update HW RTT statistics
                            lport->other_stats.hw_tx_rtt.count++;
                            lport->other_stats.hw_tx_rtt.sum_ns += delta_ns;
                            if (delta_ns < lport->other_stats.hw_tx_rtt.min_ns)
                                lport->other_stats.hw_tx_rtt.min_ns = delta_ns;
                            if (delta_ns > lport->other_stats.hw_tx_rtt.max_ns)
                                lport->other_stats.hw_tx_rtt.max_ns = delta_ns;
                        } else {
                            lport->other_stats.hw_tx_rtt_invalid++;
                            if (_btst(DEBUG_MODE))
                                printf("Warning: HW RTT too large: %'" PRIu64 " ns\n", delta_ns);
                        }
                    } else {
                        lport->other_stats.rx_timestamp_errors++;
                        if (_btst(DEBUG_MODE))
                            printf("Warning: RX timestamp < TX timestamp\n");
                    }
                }
            }
            // Swap MAC addresses
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr, &tmp);
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr,
                                &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr);
            rte_ether_addr_copy(&tmp, &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr);

            tx_mbufs[nb_tx++] = pkt;
        }
    }

    // Client mode - free the Rx packets
    if (pinfo->client_mode)
        rte_pktmbuf_free_bulk(tx_mbufs, nb_tx);
    else        // Server mode - loopback packets
        send_packets(lport->pid, lport->qid, tx_mbufs, nb_tx);
    lport->other_stats.total_pkts.tx += nb_tx;
    return 0;
}

int
rxtx_routine(void *arg __rte_unused)
{
    uint16_t pid         = pinfo->lport_idx++;
    lport_t *lport       = &pinfo->lports[pid];
    uint64_t tx_begin_ns = 0;
    uint64_t curr_ns     = 0;

    printf("Starting RX/TX on port %u on lcore %u\n", pid, rte_lcore_id());
    lport->pid          = pid;
    lport->qid          = 0;
    lport->tx_timestamp = UINT64_MAX;        // Initialize to invalid value
    if (port_init(lport) < 0) {
        printf("Cannot init lport %u:%u\n", pid, lport->qid);
        goto leave;
    }

    while (is_link_up(pid) == false) {
        usleep(2500);
        if (!is_running())
            goto leave;
    }

    _bset(RESET_STATS);        // Reset stats at start of test

    curr_ns     = clock_get_ns();
    tx_begin_ns = curr_ns + pinfo->tx_interval_ns;        // Start after 1 interval

    /* Run until the application has stopped or been killed. */
    while (is_running()) {
        curr_ns = clock_get_ns();
        /* Wait until the next cycle time */
        if (curr_ns >= tx_begin_ns) {
            tx_begin_ns = curr_ns + pinfo->tx_interval_ns;

            if (pinfo->client_mode && tx_timestamping(lport) < 0) {
                printf("Failed to send packet on port %u\n", lport->pid);
                break;
            }
        }

        if (rx_timestamping(lport) < 0)
            break;
    }
leave:
    stop_running();

    return 0;
}

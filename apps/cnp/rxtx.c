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
#include <rte_cycles.h>
#include <unistd.h>

/**
 * The structure of a PTP V2 packet.
 *
 * Only the minimum fields used by the ieee1588 test are represented.
 */
struct ptpv2_msg {
    uint8_t msg_id;
    uint8_t version; /**< must be 0x02 */
};

#define TX_READ_TIMESTAMP_TIMO 1000        // 1000 iterations timeout
#define TX_POLL_DELAY_US       1           // 1 microseconds delay between polls

static int
poll_tx_timestamp(uint16_t port_id, uint64_t *tx_timestamp, stats_t *stats)
{
    struct timespec timestamp = {0};
    int timo                  = TX_READ_TIMESTAMP_TIMO;
    int ret                   = -EINVAL;

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
        } else if (ret == -1 || ret == -EINVAL || ret == -EAGAIN) {
            // Timestamp not yet available (-1, -EINVAL, -EAGAIN all mean "not ready yet")
            // ICE driver often returns -1 instead of proper errno codes
            rte_delay_us(TX_POLL_DELAY_US);
        } else {
            // Unexpected error code (e.g., -ENODEV, -EIO)
            if (_btst(DEBUG_MODE))
                printf("TX timestamp read error: ret=%d (%s)\n", ret, rte_strerror(-ret));
            stats->tx_timestamp_errors++;
            break;
        }
    } while (--timo > 0);

    if ((ret == -1 || ret == -EINVAL || ret == -EAGAIN) && timo == 0) {
        stats->tx_timestamp_timeouts++;
        // if (_btst(DEBUG_MODE))
        printf("TX timestamp timeout after %d attempts\n", TX_READ_TIMESTAMP_TIMO);
    }

    return ret;
}

static inline int
tx_timestamping(lport_t *lport)
{
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth;
    struct ptpv2_msg *ptp_hdr;

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

    ptp_hdr          = rte_pktmbuf_mtod_offset(m, struct ptpv2_msg *, sizeof(struct rte_ether_hdr));
    ptp_hdr->msg_id  = 0x0;         // PTP_SYNC_MESSAGE
    ptp_hdr->version = 0x02;        // PTP v2

    if (_btst(SW_TIMESTAMP)) {
        probe_payload_t *payload = rte_pktmbuf_mtod_offset(
            m, probe_payload_t *, sizeof(struct rte_ether_hdr) + sizeof(struct ptpv2_msg));

        memset(payload, 0, sizeof(probe_payload_t));
        payload->magic           = rte_cpu_to_be_16(THE_MAGIC);
        payload->packet_type     = rte_cpu_to_be_16(TYPE_PROBE_SEND);
        payload->sequence_number = rte_cpu_to_be_32(lport->tx_sequence);
		lport->tx_sequence++;

        payload->T1 =
            rte_cpu_to_be_64(start_stats_timer(&lport->other_stats.sw_rtt, clock_get_ns()));
		if (_btst(DEBUG_MODE))
			printf("Transmit probe seq=%u T1=%'" PRIu64 " ns\n",
               rte_be_to_cpu_32(payload->sequence_number), rte_be_to_cpu_64(payload->T1));
	}

    m->ol_flags = 0;

    // Set TX timestamp flag if enabled
    if (_btst(HW_TIMESTAMP))
        m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;

    send_packets(lport->pid, lport->qid, &m, 1);

    if (_btst(HW_TIMESTAMP)) {
        int ret;

        lport->tx_timestamp = UINT64_MAX;        // Invalidate previous TX timestamp

        start_stats_timer(&lport->other_stats.poll, clock_get_ns());
        ret = poll_tx_timestamp(lport->pid, &lport->tx_timestamp, &lport->other_stats);
        if (ret != 0) {
            // Keep previous timestamp on failure
            if (_btst(DEBUG_MODE))
                printf("Failed to get TX timestamp on port %u: %s\n", lport->pid,
                       rte_strerror(-ret));
        }
        end_stats_timer(&lport->other_stats.poll, clock_get_ns());
    }

    return 0;
}

static inline int
rx_timestamping(lport_t *lport)
{
    struct rte_mbuf **rx_mbufs = lport->rx_mbufs;
    struct rte_mbuf **tx_mbufs = lport->tx_mbufs;
    struct rte_ether_addr tmp;
    uint64_t T1;
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
				uint32_t sequence_number;

                payload = (probe_payload_t *)(rte_pktmbuf_mtod_offset(
                    pkt, char *, sizeof(struct rte_ether_hdr) + sizeof(struct ptpv2_msg)));

                if (rte_be_to_cpu_16(payload->magic) != THE_MAGIC) {
                    lport->other_stats.rx_no_probe_frames++;
                    printf("Invalid magic: 0x%04x\n", rte_be_to_cpu_16(payload->magic));
                    rte_pktmbuf_free(pkt);
                    continue;
                }
				sequence_number = rte_be_to_cpu_32(payload->sequence_number);
				if (sequence_number != lport->rx_sequence) {
					printf("Out-of-order probe received: seq=%u expected>= %u\n",
						   sequence_number, lport->rx_sequence);
					lport->rx_sequence = sequence_number;
					rte_pktmbuf_free(pkt);
					continue;
				}
				lport->rx_sequence++;

                T1 = rte_be_to_cpu_64(payload->T1);
				if (_btst(DEBUG_MODE))
	                printf("Received probe seq=%u T1=%'" PRIu64 " Now=%'" PRIu64 " ns\n",
                       rte_be_to_cpu_32(payload->sequence_number), T1, lport->other_stats.sw_rtt.start_ns);
                end_stats_timer(&lport->other_stats.sw_rtt, clock_get_ns());
            }

            if (_btst(HW_TIMESTAMP)) {
                // Check packet for RX timestamp flag
                if (pkt->ol_flags & lport->rx_timestamp_flag) {
                    uint64_t rx_timestamp =
                        *RTE_MBUF_DYNFIELD(pkt, lport->rx_timestamp_offset, uint64_t *);

                    // Validate RX timestamp is non-zero and reasonable
                    if (rx_timestamp == 0) {
                        lport->other_stats.rx_timestamp_errors++;
                        if (_btst(DEBUG_MODE))
                            printf("Warning: RX timestamp is zero\n");
                    } else {
                        if (lport->tx_timestamp != UINT64_MAX &&
                            lport->tx_timestamp > rx_timestamp) {
                            lport->other_stats.hw_rx_rtt_invalid++;
                            if (_btst(DEBUG_MODE))
                                printf("Warning: RX timestamp less than TX timestamp\n");
                            rte_pktmbuf_free(pkt);
                            continue;
                        } else
                            end_stats_timer(&lport->other_stats.hw_rtt, rx_timestamp);
                    }
                } else
                    lport->other_stats.rx_no_timestamp++;
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

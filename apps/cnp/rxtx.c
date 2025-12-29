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

#define RX_BURST_SIZE 16

static inline int
tx_timestamping(lport_t *lport)
{
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth;

    if ((m = rte_pktmbuf_alloc(lport->tx_mp)) == NULL) {
        lport->other_stats.no_mbufs++;
        return -1;
    }

    rte_pktmbuf_pkt_len(m)  = pinfo->pkt_length;
    rte_pktmbuf_data_len(m) = pinfo->pkt_length;
    eth                     = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    rte_ether_addr_copy(&lport->src_mac, &eth->src_addr);
    rte_ether_addr_copy(&lport->dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_1588);

    probe_payload_t *payload =
        (probe_payload_t *)(rte_pktmbuf_mtod_offset(m, char *, sizeof(struct rte_ether_hdr)));

    memset(payload, 0, sizeof(probe_payload_t));
    payload->magic           = rte_cpu_to_be_16(THE_MAGIC);
    payload->sequence_number = rte_cpu_to_be_16(lport->tx_sequence++);
    payload->packet_type     = TYPE_PROBE_SEND;
    payload->T1              = clock_get_ns();        // Example timestamp

    if (rte_eth_tx_burst(lport->pid, lport->qid, &m, 1) == 0) {
		rte_pktmbuf_free(m);
		lport->other_stats.tx_frame_errors++;
		printf("Failed to send packet on port %u\n", lport->pid);
		return -1;
	}
    lport->other_stats.total_pkts.tx++;

    return 0;
}

static inline int
rx_timestamping(lport_t *lport)
{
    struct rte_mbuf *mbuf[RX_BURST_SIZE];
    struct rte_ether_addr tmp;
    uint16_t nb_rx;

    if ((nb_rx = rte_eth_rx_burst(lport->pid, lport->qid, mbuf, RX_BURST_SIZE)) > 0) {
        struct rte_ether_hdr *eth_hdr;
        probe_payload_t *payload;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *pkt = mbuf[i];

            if (pkt == NULL)
                break;

            // Process each received packet
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
                printf("Unknown EtherType: 0x%04x\n", rte_be_to_cpu_16(eth_hdr->ether_type));
                rte_pktmbuf_free(pkt);
                continue;
            }
            payload = (probe_payload_t *)(rte_pktmbuf_mtod_offset(pkt, char *,
                                                                  sizeof(struct rte_ether_hdr)));
            if (rte_be_to_cpu_16(payload->magic) != THE_MAGIC) {
                lport->other_stats.rx_no_probe_frames++;
                printf("Invalid magic: 0x%04x\n", rte_be_to_cpu_16(payload->magic));
                rte_pktmbuf_free(pkt);
                continue;
            }
            // Swap MAC addresses
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr, &tmp);
            rte_ether_addr_copy(&rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr,
                                &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->dst_addr);
            rte_ether_addr_copy(&tmp, &rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *)->src_addr);

			if (rte_eth_tx_burst(lport->pid, lport->qid, &pkt, 1) == 0) {
				rte_pktmbuf_free(pkt);
				lport->other_stats.tx_frame_errors++;
				printf("Failed to send mirror packet on port %u\n", lport->pid);
			}
        }
    }

    return 0;
}

int
rxtx_routine(void *arg __rte_unused)
{
    uint16_t pid            = pinfo->lport_idx++;
    lport_t *lport          = &pinfo->lports[pid];
    uint64_t tx_begin_ns    = 0;
    timestamping_fn rx_func = rx_timestamping, tx_func = tx_timestamping;

    lport->pid = pid;
    lport->qid = 0;
    if (port_init(lport) < 0)
        rte_exit(EXIT_FAILURE, "Cannot init lport %u:%u\n", lport->pid, lport->qid);

    while (is_link_up(lport->pid) == false) {
        usleep(250000);
        if (!is_running())
            return 0;
    }

    tx_begin_ns = clock_get_ns() + pinfo->tx_interval_ns;

    /* Run until the application has stopped or been killed. */
    while (is_running()) {
        /* Wait until the next cycle time */
        if (clock_get_ns() >= tx_begin_ns) {
            tx_begin_ns += pinfo->tx_interval_ns;

            if (tx_func(lport) < 0)
                rte_exit(EXIT_FAILURE, "failed to send packet on port %u", lport->pid);
        }

        if (rx_func(lport) < 0)
            stop_running();
    }

    return 0;
}

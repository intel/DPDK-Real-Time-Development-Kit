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

static inline int
tx_timestamping(lport_t *lport)
{
	lcore_t *lcore = (lcore_t *)&pinfo->lcores[rte_lcore_id()];
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth;

    if ((m = rte_pktmbuf_alloc(lport->tx_mp)) == NULL) {
        lcore->stats.no_mbufs++;
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

    send_packets(lport->pid, lport->qid, &m);
    lcore->stats.total_pkts.tx++;

    return 0;
}

static inline int
rx_timestamping(lport_t *lport)
{
	lcore_t *lcore = (lcore_t *)&pinfo->lcores[rte_lcore_id()];
    struct rte_mbuf *mbuf;
    struct rte_ether_addr tmp;

    if (rte_eth_rx_burst(lport->pid, lport->qid, &mbuf, 1) > 0) {
        struct rte_ether_hdr *eth_hdr;
        probe_payload_t *payload;
        lcore->stats.total_pkts.rx++;

        if (rte_pktmbuf_pkt_len(mbuf) < sizeof(struct rte_ether_hdr) + sizeof(probe_payload_t)) {
            lcore->stats.rx_frame_errors++;
            rte_pktmbuf_free(mbuf);
            return 0;
        }

        eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_1588)) {
            lcore->stats.rx_unknown_frames++;
            rte_pktmbuf_free(mbuf);
            return 0;
        }
        payload = (probe_payload_t *)(rte_pktmbuf_mtod_offset(mbuf, char *,
                                                              sizeof(struct rte_ether_hdr)));
        if (rte_be_to_cpu_16(payload->magic) != THE_MAGIC) {
            lcore->stats.rx_no_probe_frames++;
            rte_pktmbuf_free(mbuf);
            return 0;
        }
        // Swap MAC addresses
        rte_ether_addr_copy(&rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *)->dst_addr, &tmp);
        rte_ether_addr_copy(&rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *)->src_addr,
                            &rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *)->dst_addr);
        rte_ether_addr_copy(&tmp, &rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *)->src_addr);

        while (is_running() && rte_eth_tx_burst(lport->pid, lport->qid, &mbuf, 1) == 0)
            ;
        lcore->stats.total_pkts.tx++;
    }
    return 0;
}

int
rxtx_routine(void *arg __rte_unused)
{
    uint16_t lid            = rte_lcore_id();
    lcore_t *lcore          = &pinfo->lcores[lid];
    uint16_t pid            = pinfo->lport_idx++;
    lport_t *lport          = &pinfo->lports[pid];
    uint64_t tx_begin_ns    = 0;
    timestamping_fn rx_func = rx_timestamping, tx_func = tx_timestamping;

    lcore->lcore_id = lid;
    lcore->valid    = 1;
    lport->pid      = pid;
    lport->qid      = 0;
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

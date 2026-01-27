/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <unistd.h>
#include "launch-time.h"
#include "log.h"
#include "mqtt.h"

static inline int
tx_timestamping(lcore_t *lcore, uint16_t pid, uint16_t qid)
{
    struct rte_mbuf **mbufs = lcore->tx_mbufs;
    lport_t *lport          = &lcore->lport;
    struct rte_ether_hdr *eth;
    uint64_t port_ns = 0;

    if (rte_mempool_get_bulk(lport->tx_mp, (void **)mbufs, pinfo->burst_count) != 0) {
        lcore->stats.no_mbufs++;
        return -1;
    }

    if (_btst(LAUNCH_TIME))
        port_ns = port_clock_get_ns(pid);

    for (uint16_t i = 0; i < pinfo->burst_count; i++) {
        struct rte_mbuf *m = mbufs[i];

        rte_pktmbuf_pkt_len(m)  = pinfo->pkt_length;
        rte_pktmbuf_data_len(m) = pinfo->pkt_length;
        eth                     = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        rte_ether_addr_copy(&lport->src_mac, &eth->src_addr);
        rte_ether_addr_copy(&lport->dst_mac, &eth->dst_addr);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_1588);

        // Send a launch time timestamp if enabled only on the first packet of the burst
        if (_btst(LAUNCH_TIME) && i == 0) {
            m->ol_flags |= pinfo->tx_timestamp_flag;
            m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;
            *RTE_MBUF_DYNFIELD(m, pinfo->tx_timestamp_offset, uint64_t *) =
                port_ns;
        } else {
            m->ol_flags &= ~pinfo->tx_timestamp_flag;
            m->ol_flags &= ~RTE_MBUF_F_TX_IEEE1588_TMST;
        }
    }
    send_packets(pid, qid, mbufs, pinfo->burst_count);
    lcore->stats.total_pkts.tx += pinfo->burst_count;

    return 0;
}

static uint64_t prev_rx;

void
reset_rx_timestamp(void)
{
    prev_rx = 0;
}

static inline int
rx_timestamping(lcore_t *lcore, uint16_t pid, uint16_t qid)
{
    struct rte_mbuf **mbufs;
    uint16_t nb_rx, total_rx, to_recv;
    uint64_t curr_ns;

    mbufs   = lcore->rx_mbufs;
    to_recv = pinfo->burst_count;
    total_rx     = 0;

    if ((nb_rx = rte_eth_rx_burst(pid, qid, mbufs, to_recv)) > 0) {
        to_recv -= nb_rx;
        total_rx += nb_rx;

        // Process packets only after last packet received for the cycle.
        if (likely(to_recv == 0)) {
            lcore->stats.total_pkts.rx += total_rx;

			// Check only the first packet for timestamp data else use system clock
            if (mbufs[0]->ol_flags & pinfo->rx_timestamp_flag)
                curr_ns = *RTE_MBUF_DYNFIELD(mbufs[0], pinfo->rx_timestamp_offset, uint64_t *);
            else
                curr_ns = clock_get_ns();
            if (prev_rx == 0)
                prev_rx = curr_ns;
            else {
                min_avg_max_update(&lcore->stats.launch_time, curr_ns - prev_rx);
                prev_rx = curr_ns;
            }
        }

        rte_pktmbuf_free_bulk(mbufs, total_rx);
    }
    return 0;
}

int
rxtx_routine(void *arg)
{
    lcore_t *lcore          = arg;
    lport_t *lport          = &lcore->lport;
    uint16_t pid            = lport->pid;
    uint16_t qid            = lport->qid;
    uint64_t tx_begin_ns    = 0;
    timestamping_fn rx_func = rx_timestamping, tx_func = tx_timestamping;

    while (is_link_up(pid) == false) {
        usleep(250000);
        if (!is_running())
            return 0;
    }
    sleep_sec(pinfo->delay_sec);        // Wait for specified delay before starting the test

    tx_begin_ns = clock_get_ns() + pinfo->launch_interval_ns;

    /* Run until the application has stopped or been killed. */
    while (is_running()) {
        /* Wait until the next cycle time */
        if (clock_get_ns() >= (tx_begin_ns - pinfo->tx_burst_offset_ns)) {
            tx_begin_ns += pinfo->launch_interval_ns;

            if (tx_func(lcore, pid, qid))
                rte_exit(EXIT_FAILURE, "failed to send packets on port %u\n", lport->pid);
        }

        if (rx_func(lcore, pid, qid))
            stop_running();
    }

    return 0;
}

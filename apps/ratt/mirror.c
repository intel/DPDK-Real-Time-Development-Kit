/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "ratt.h"
#include "mqtt.h"

static inline void
update_mac_addrs(lport_t *lport, struct rte_mbuf **mbufs, uint16_t nb_pkts)
{
    // Update the Source/Destination MAC addresses
    for (uint16_t i = 0; i < nb_pkts; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbufs[i], struct rte_ether_hdr *);

        rte_ether_addr_copy(&lport->src_mac, &eth->src_addr);
        rte_ether_addr_copy(&lport->dst_mac, &eth->dst_addr);
    }
}

int
mirror_routine(void *arg)
{
    lcore_t *lcore = arg;
    lport_t *lport = &lcore->lport;
    uint16_t pid   = lport->pid;
    uint16_t qid   = lport->qid;
    uint64_t begin_ns = 0;
    uint64_t begin_wl_ns = 0;
    uint64_t delta_ns = 0;

    // wait for the link to become up before starting the mirror processing
    while (is_link_up(pid) == false) {
        rte_delay_us(50);
        if (!is_running())
            return 0;
    }

    if (pinfo->rt_workload.enabled)
        lcore->stats.workload.min_ns = BIG_NUM;
        
    /* Run until the application has quit or was killed. */
    if (pinfo->mirror_serial_enabled) {
        struct rte_mbuf **pkts, **mbufs;
        uint16_t nb_rx, to_recv, total_rx;

        mbufs = lcore->rx_mbufs;

        while (is_running()) {
            pkts     = mbufs;
            to_recv  = pinfo->burst_count;
            total_rx = 0;
            begin_time(&begin_ns);
            do {
                if ((nb_rx = rte_eth_rx_burst(pid, qid, pkts, to_recv))) {
                    to_recv -= nb_rx;
                    pkts += nb_rx;
                    total_rx += nb_rx;

                    if (likely(to_recv == 0)) {
                        lcore->stats.total_pkts.rx += total_rx;
                        lcore->stats.total_pkts.tx += total_rx;
						goto record;
                    }
                }
            } while (is_running());
            lcore->stats.rx_timeout++;
record:
			end_time(&lcore->stats.rx_snapshot, begin_ns);

            if (pinfo->rt_workload.enabled)
            {
                begin_wl_ns = clock_get_ns();
                pinfo->rt_workload.workload_function(pinfo->rt_workload.workload_argc, pinfo->rt_workload.workload_argv);
                delta_ns = clock_get_ns() - begin_wl_ns;
                min_avg_max_update(&lcore->stats.workload, delta_ns);
                min_avg_max_update(&lcore->stats.workload_snapshot, delta_ns);
            }

            begin_time(&begin_ns);
            update_mac_addrs(lport, mbufs, total_rx);
            send_packets(pid, qid, mbufs, total_rx);
            end_time(&lcore->stats.tx_snapshot, begin_ns);
        }
    } else {
        struct rte_mbuf **mbufs = lcore->rx_mbufs;
        uint16_t nb_rx;

        while (is_running()) {
			begin_time(&begin_ns);
            if ((nb_rx = rte_eth_rx_burst(pid, qid, mbufs, MAX_BURST_COUNT)) == 0)
                continue;
			end_time(&lcore->stats.rx_snapshot, begin_ns);

            lcore->stats.total_pkts.rx += nb_rx;
            lcore->stats.total_pkts.tx += nb_rx;
			begin_time(&begin_ns);
            update_mac_addrs(lport, mbufs, nb_rx);
            send_packets(pid, qid, mbufs, nb_rx);
			end_time(&lcore->stats.tx_snapshot, begin_ns);
        }
    }

    return 0;
}

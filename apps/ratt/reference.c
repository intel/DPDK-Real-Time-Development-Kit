/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "ratt.h"
#include "log.h"
#include "mqtt.h"
#include <rte_hexdump.h>

static struct rte_ipv4_hdr pkt_ip_hdr; /**< IP header of transmitted packets. */
static struct rte_udp_hdr pkt_udp_hdr; /**< UDP header of tx packets. */
/* use RFC863 Discard Protocol */
uint16_t tx_udp_src_port = 9;
uint16_t tx_udp_dst_port = 9;

/* use RFC5735 / RFC2544 reserved network test addresses */
uint32_t tx_ip_src_addr = (198U << 24) | (18 << 16) | (0 << 8) | 1;
uint32_t tx_ip_dst_addr = (198U << 24) | (18 << 16) | (0 << 8) | 2;

#define IP_DEFTTL 64 /* from RFC 1340. */

static void
setup_pkt_udp_ip_headers(struct rte_ipv4_hdr *ip_hdr, struct rte_udp_hdr *udp_hdr,
                         uint16_t pkt_data_len)
{
    uint16_t pkt_len;

    /*
     * Initialize UDP header.
     */
    pkt_len              = (uint16_t)(pkt_data_len + sizeof(struct rte_udp_hdr));
    udp_hdr->src_port    = rte_cpu_to_be_16(tx_udp_src_port);
    udp_hdr->dst_port    = rte_cpu_to_be_16(tx_udp_dst_port);
    udp_hdr->dgram_len   = rte_cpu_to_be_16(pkt_len);
    udp_hdr->dgram_cksum = 0; /* No UDP checksum. */

    /*
     * Initialize IP header.
     */
    pkt_len                 = (uint16_t)(pkt_len + sizeof(struct rte_ipv4_hdr));
    ip_hdr->version_ihl     = RTE_IPV4_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_UDP;
    ip_hdr->packet_id       = 0;
    ip_hdr->total_length    = rte_cpu_to_be_16(pkt_len);
    ip_hdr->src_addr        = rte_cpu_to_be_32(tx_ip_src_addr);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(tx_ip_dst_addr);

    /*
     * Compute IP header checksum.
     */
    ip_hdr->hdr_checksum = rte_ipv4_cksum_simple(ip_hdr);
}

static inline int
tx_timestamping(lcore_t *lcore, uint16_t pid, uint16_t qid)
{
    struct rte_mbuf **mbufs = lcore->tx_mbufs;
    lport_t *lport          = &lcore->lport;
    uint64_t curr_ns;

    if (rte_mempool_get_bulk(lport->tx_mp, (void **)mbufs, pinfo->burst_count) != 0) {
        lcore->stats.no_mbufs++;
        return -1;
    }

    setup_pkt_udp_ip_headers(&pkt_ip_hdr, &pkt_udp_hdr, pinfo->pkt_length);

    curr_ns = clock_get_ns();
    for (uint16_t i = 0; i < pinfo->burst_count; i++) {
        struct rte_mbuf *m        = mbufs[i];
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip   = (struct rte_ipv4_hdr *)&eth[1];
        struct rte_udp_hdr *udp   = (struct rte_udp_hdr *)&ip[1];
        timestamp_t *ts           = (timestamp_t *)&udp[1];

        rte_pktmbuf_pkt_len(m)  = pinfo->pkt_length;
        rte_pktmbuf_data_len(m) = pinfo->pkt_length;

        rte_ether_addr_copy(&lport->src_mac, &eth->src_addr);
        rte_ether_addr_copy(&lport->dst_mac, &eth->dst_addr);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        rte_memcpy(ip, &pkt_ip_hdr, sizeof(pkt_ip_hdr));
        rte_memcpy(udp, &pkt_udp_hdr, sizeof(pkt_udp_hdr));

        ts->timestamp = curr_ns;
        ts->beef      = THE_BEEF_TIMESTAMP;
        ts->id        = lcore->tx_id++;
    }
    send_packets(pid, qid, mbufs, pinfo->burst_count);
    lcore->stats.total_pkts.tx += pinfo->burst_count;

    return 0;
}

static inline int
rx_timestamping(lcore_t *lcore, uint16_t pid, uint16_t qid)
{
    struct rte_mbuf **mbufs, **pkts;
    timestamp_t *ts;
    uint64_t curr_ns, end_ns, begin_ns = 0;
    uint16_t nb_rx, total_rx, to_recv;

    pkts = mbufs = lcore->rx_mbufs;
    to_recv      = pinfo->burst_count;
    end_ns       = lcore->end_cycle_ns;
    total_rx     = 0;

    begin_time(&begin_ns);
    // Attempt to receive burst_count number of packets inside cycle time.
    do {
        if ((nb_rx = rte_eth_rx_burst(pid, qid, pkts, to_recv)) > 0) {
            pkts += nb_rx;
            to_recv -= nb_rx;
            total_rx += nb_rx;

            // Process packets only after last packet received for the cycle.
            if (likely(to_recv == 0))
                goto process_packets;
        }
    } while (clock_get_ns() < end_ns);

    lcore->stats.rx_try_extra_time++;        // try a bit more time before timeout.
    end_ns += (pinfo->cycle_time_ns / 4);
    do {
        if ((nb_rx = rte_eth_rx_burst(pid, qid, pkts, to_recv)) > 0) {
            pkts += nb_rx;
            to_recv -= nb_rx;
            total_rx += nb_rx;

            // Process packets only after last packet received for the cycle.
            if (likely(to_recv == 0))
                goto process_packets;
        }
    } while (clock_get_ns() < end_ns);

    if (!is_running() || total_rx == 0)
        return 0UL;

process_packets:
    end_time(&lcore->stats.rx_snapshot, begin_ns);

    curr_ns = clock_get_ns();
    lcore->stats.total_pkts.rx += total_rx;
    if (total_rx && (lcore->skip_cnt && lcore->skip_cnt-- > 0)) {
        rte_pktmbuf_free_bulk(mbufs, total_rx);
        lcore->rx_id += total_rx;
        return 0UL;
    }

    ts = rte_pktmbuf_mtod_offset(mbufs[0], timestamp_t *,
                                 sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                                     sizeof(struct rte_udp_hdr));

    if (ts->beef == THE_BEEF_TIMESTAMP) {
        if (lcore->rx_id != ts->id)
            lcore->stats.id_error++;
        lcore->rx_id += total_rx;

        uint64_t delta_ns = curr_ns - ts->timestamp;

        min_avg_max_update(&lcore->stats.rtt, delta_ns);
        min_avg_max_update(&lcore->stats.snapshot, delta_ns);
        if (delta_ns >= (pinfo->cycle_time_ns * 2))
            min_avg_max_update(&lcore->stats.spike, delta_ns);

        log_delta(delta_ns);
        mqtt_delta(delta_ns);
    } else
        lcore->stats.no_timestamp++;

    rte_pktmbuf_free_bulk(mbufs, total_rx);

    return 0UL;
}

int
reference_routine(void *arg)
{
    lcore_t *lcore    = arg;
    lport_t *lport    = &lcore->lport;
    uint16_t pid      = lport->pid;
    uint16_t qid      = lport->qid;
    uint64_t begin_ns = 0;

    while (!is_link_up(pid)) {
        sleep_sec(1);        // Wait for 1 second before checking the link status again
        if (!is_running())
            return 0;
    }
    sleep_sec(pinfo->delay_sec);        // Wait for specified delay before starting the test

    lcore->end_cycle_ns = clock_get_ns();

    lcore->skip_cnt = pinfo->pkt_skip_cnt;

    /* Run until the application has stopped or been killed. */
    while (is_running()) {
        if (clock_get_ns() >= lcore->end_cycle_ns) {
            lcore->end_cycle_ns += pinfo->cycle_time_ns;

            begin_time(&begin_ns);
            if (tx_timestamping(lcore, pid, qid))
                rte_exit(EXIT_FAILURE, "failed to send packets on port %u", lport->pid);
            end_time(&lcore->stats.tx_snapshot, begin_ns);

            if (rx_timestamping(lcore, pid, qid)) {
                if (!pinfo->continue_on_error) {
                    end_time(&lcore->stats.rx_snapshot, begin_ns);
                    stop_running();
                }
            }
        }
    }

    return 0;
}

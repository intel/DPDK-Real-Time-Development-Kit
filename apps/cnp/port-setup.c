/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"

#include <rte_mbuf_dyn.h>

static struct rte_eth_conf default_port_conf = {
    .rxmode =
        {
            .mq_mode          = RTE_ETH_MQ_RX_RSS,
            .max_lro_pkt_size = RTE_ETHER_MAX_LEN,
            .offloads         = RTE_ETH_RX_OFFLOAD_CHECKSUM,
        },

    .rx_adv_conf =
        {
            .rss_conf =
                {
                    .rss_key = NULL,
                    .rss_hf  = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP |
                              RTE_ETH_RSS_SCTP | RTE_ETH_RSS_L2_PAYLOAD,
                },
        },
    .txmode =
        {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
    .intr_conf =
        {
            .lsc = 0,
        },
};

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
int
port_init(lport_t *lport)
{
    struct rte_eth_dev_info dev_info = {0};
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1;
    const uint16_t tx_rings = 1;
    int retval;
    uint16_t nb_rxd = DEFAULT_RING_SIZE;
    uint16_t nb_txd = DEFAULT_RING_SIZE;
	uint16_t pid, qid;
    int socket_id;

	pid = lport->pid;
	qid = lport->qid;
    if (!rte_eth_dev_is_valid_port(pid))
        rte_exit(EXIT_FAILURE, "Invalid port %u\n", pid);

    retval = rte_eth_dev_info_get(pid, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u:%u) info: %s\n", pid, qid, strerror(-retval));
        return retval;
    }
    socket_id = rte_eth_dev_socket_id(pid);

    /* Get a clean copy of the configuration structure */
    rte_memcpy(&port_conf, &default_port_conf, sizeof(struct rte_eth_conf));

    lport->rx_mp = lport_pktmbuf_pool("rx", pid, qid, DEFAULT_MBUF_COUNT, DEFAULT_MBUF_SIZE,
                                      DEFAULT_CACHE_SIZE);
    if (lport->rx_mp == NULL)
        rte_exit(EXIT_FAILURE, "Error during allocating mbuf pool for RX\n");

    lport->tx_mp = lport_pktmbuf_pool("tx", pid, qid, DEFAULT_MBUF_COUNT, DEFAULT_MBUF_SIZE,
                                      DEFAULT_CACHE_SIZE);
    if (lport->tx_mp == NULL)
        rte_exit(EXIT_FAILURE, "Error during allocating mbuf pool for TX\n");

    port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
    port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    if (dev_info.max_rx_queues == 1)
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    if (dev_info.max_vfs) {
        if (port_conf.rx_adv_conf.rss_conf.rss_hf != 0)
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_VMDQ_RSS;
    }
    if (*dev_info.dev_flags & RTE_ETH_DEV_INTR_LSC)
        port_conf.intr_conf.lsc = 1;

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(pid, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(pid, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (uint16_t q = 0; q < rx_rings; q++) {
        struct rte_eth_rxconf rxconf;

        rxconf                   = dev_info.default_rxconf;
        rxconf.offloads          = port_conf.rxmode.offloads;
        rxconf.rx_thresh.pthresh = 0;
        rxconf.rx_thresh.wthresh = 0;
        rxconf.rx_thresh.hthresh = 0;

        retval = rte_eth_rx_queue_setup(pid, q, nb_rxd, socket_id, &rxconf, lport->rx_mp);

        if (retval < 0)
            return retval;
    }

    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (uint16_t q = 0; q < tx_rings; q++) {
        struct rte_eth_txconf txconf;

        txconf                   = dev_info.default_txconf;
        txconf.offloads          = port_conf.txmode.offloads;
        txconf.tx_thresh.pthresh = 0;
        txconf.tx_thresh.wthresh = 0;
        txconf.tx_thresh.hthresh = 0;

        retval = rte_eth_tx_queue_setup(pid, q, nb_txd, socket_id, &txconf);
        if (retval < 0)
            return retval;
    }

    // Convert the MAC address string into binary format
    if (rte_eth_macaddr_get(pid, &lport->src_mac) < 0)
        rte_exit(EXIT_FAILURE, "Can't get MAC address on port=%u : %s\n", pid,
                 rte_strerror(rte_errno));

    char buff[64];
    rte_ether_unformat_addr(pinfo->dst_mac_str, &lport->dst_mac);
    rte_ether_format_addr(buff, sizeof(buff), &lport->dst_mac);
    printf("Port %u Dst MAC %s ", pid, buff);

    rte_ether_format_addr(buff, sizeof(buff), &lport->src_mac);
    printf("Src MAC %s\n", buff);

    if (rte_eth_dev_set_ptypes(pid, RTE_PTYPE_UNKNOWN, NULL, 0) < 0)
        rte_exit(EXIT_FAILURE, "Port %u, Failed to disable Ptype parsing\n", pid);

    /* Start the Ethernet port. */
    if ((retval = rte_eth_dev_start(pid)) < 0)
        return retval;

    /* Enable RX in promiscuous mode for the Ethernet device. */
    if (_btst(PROMISCUOUS)) {
        retval = rte_eth_promiscuous_enable(pid);
        if (retval != 0) {
            printf("Promiscuous mode enable failed: %s\n", rte_strerror(-retval));
            return retval;
        }
    }

    return 0;
}

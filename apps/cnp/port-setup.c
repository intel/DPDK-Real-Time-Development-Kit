/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"

#include <rte_mbuf_dyn.h>

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
int
port_init(lport_t *lport)
{
    struct rte_eth_dev_info dev_info = {0};
    struct rte_eth_conf port_conf;
    const uint16_t rx_queues = 1;
    const uint16_t tx_queues = 1;
    uint16_t nb_rxd          = NUM_RX_DESC_DEFAULT;
    uint16_t nb_txd          = NUM_TX_DESC_DEFAULT;
    int retval;
    uint16_t pid, qid;
    int socket_id;

    rte_spinlock_lock(&pinfo->port_lock);

    pid = lport->pid;
    qid = lport->qid;
    if (!rte_eth_dev_is_valid_port(pid))
        rte_exit(EXIT_FAILURE, "Invalid port %u\n", pid);

    printf("Initializing port %u:%u...\n", pid, qid);
    retval = rte_eth_dev_info_get(pid, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u:%u) info: %s\n", pid, qid, strerror(-retval));
        goto err_exit;
    }
    socket_id = rte_eth_dev_socket_id(pid);

    printf("Port %u information:\n", pid);
    memset(&port_conf, 0, sizeof(struct rte_eth_conf));
    port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
    port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;

    if (dev_info.max_rx_queues == 1)
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    else
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;

    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    printf("  max_rx_queues: %u, max_tx_queues: %u\n", dev_info.max_rx_queues,
           dev_info.max_tx_queues);
    printf("  max_rx_pktlen: %u\n", dev_info.max_rx_pktlen);
    printf("  max_lro_pkt_size: %u\n", dev_info.max_lro_pkt_size);

    // Adjust max_lro_pkt_size if needed
    if (dev_info.max_lro_pkt_size == 0 || dev_info.max_lro_pkt_size >= RTE_ETHER_MAX_LEN)
        port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;

    printf("  Offload capabilities:\n");
    if (dev_info.max_vfs) {
        printf("    Max VFS: %u\n", dev_info.max_vfs);
        if (port_conf.rx_adv_conf.rss_conf.rss_hf != 0)
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_VMDQ_RSS;
    }
    if (*dev_info.dev_flags & RTE_ETH_DEV_INTR_LSC) {
        printf("    Supports Link Status Change interrupts\n");
        port_conf.intr_conf.lsc = 0;        // Disable for now
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        printf("    Supports TX mbuf fast free offload, enable\n");
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    if (_btst(HW_RX_TIMESTAMP) || _btst(HW_TX_TIMESTAMP)) {
        if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
            int offset    = 0;
            uint64_t flag = 0;

            port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;

            retval = rte_mbuf_dyn_rx_timestamp_register(&offset, &flag);
            if (retval < 0) {
                printf("    Warning: Failed to register Rx timestamp: %s\n", rte_strerror(-retval));
                _bclr(HW_RX_TIMESTAMP);
            } else {
                pinfo->timestamp_offset = offset;
                pinfo->timestamp_flag   = flag;
                printf("    Hardware timestamping enabled: (offset=%d, flag=0x%lx)\n", offset,
                       flag);
            }
        } else {
            printf("    Warning: Port %u does not support RX hardware timestamping\n", pid);
            _bclr(HW_RX_TIMESTAMP);
        }
    }
    if (_btst(HW_LAUNCH_TIME)) {
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP) {
            int offset    = 0;
            uint64_t flag = 0;

            port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP;

            retval = rte_mbuf_dyn_tx_timestamp_register(&offset, &flag);
            if (retval < 0) {
                printf("    Warning: Failed to register Tx launchtime: %s\n",
                       rte_strerror(-retval));
                _bclr(HW_LAUNCH_TIME);
            } else {
                pinfo->timestamp_offset = offset;
                pinfo->timestamp_flag   = flag;
                printf("    Hardware Tx launchtime enabled: (offset=%d, flag=0x%lx)\n", offset,
                       flag);
            }
        } else {
            printf("    Warning: Port %u does not support TX launchtime\n", pid);
            _bclr(HW_LAUNCH_TIME);
        }
    }

    printf("  Number of RX/TX queues: %u/%u\n", rx_queues, tx_queues);
    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(pid, rx_queues, tx_queues, &port_conf);
    if (retval != 0)
        goto err_exit;

    printf("  RX descriptor limits... min:%u max:%u align:%u\n", dev_info.rx_desc_lim.nb_min,
           dev_info.rx_desc_lim.nb_max, dev_info.rx_desc_lim.nb_align);
    if (dev_info.rx_desc_lim.nb_min > nb_rxd)
        nb_rxd = dev_info.rx_desc_lim.nb_min;
    if (dev_info.rx_desc_lim.nb_max > 0) {
        if (nb_rxd > dev_info.rx_desc_lim.nb_max)
            nb_rxd = dev_info.rx_desc_lim.nb_max;
    }
    printf("    Using %u RX descriptors\n", nb_rxd);
    printf("  TX descriptor limits... min:%u max:%u align:%u\n", dev_info.tx_desc_lim.nb_min,
           dev_info.tx_desc_lim.nb_max, dev_info.tx_desc_lim.nb_align);
    if (dev_info.tx_desc_lim.nb_min > nb_txd)
        nb_txd = dev_info.tx_desc_lim.nb_min;
    if (dev_info.tx_desc_lim.nb_max > 0) {
        if (nb_txd > dev_info.tx_desc_lim.nb_max)
            nb_txd = dev_info.tx_desc_lim.nb_max;
    }
    printf("    Using %u TX descriptors\n", nb_txd);
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(pid, &nb_rxd, &nb_txd);
    if (retval != 0)
        goto err_exit;

    lport->rx_mp = lport_pktmbuf_pool("rx", pid, qid, DEFAULT_MBUF_COUNT, DEFAULT_MBUF_SIZE,
                                      DEFAULT_CACHE_SIZE);
    if (lport->rx_mp == NULL) {
        printf("Error during allocating mbuf pool for RX\n");
        goto err_exit;
    }

    lport->tx_mp = lport_pktmbuf_pool("tx", pid, qid, DEFAULT_MBUF_COUNT, DEFAULT_MBUF_SIZE,
                                      DEFAULT_CACHE_SIZE);
    if (lport->tx_mp == NULL) {
        printf("Error during allocating mbuf pool for TX\n");
        goto err_exit;
    }

    printf("  Rx Mempool: %s\n", lport->rx_mp->name);
    printf("  Tx Mempool: %s\n", lport->tx_mp->name);

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (uint16_t q = 0; q < rx_queues; q++) {
        struct rte_eth_rxconf rxconf;

        rxconf                   = dev_info.default_rxconf;
        rxconf.offloads          = port_conf.rxmode.offloads;
        rxconf.rx_thresh.pthresh = 0;
        rxconf.rx_thresh.wthresh = 0;
        rxconf.rx_thresh.hthresh = 0;

        retval = rte_eth_rx_queue_setup(pid, q, nb_rxd, socket_id, &rxconf, lport->rx_mp);
        if (retval < 0)
            goto err_exit;
    }

    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (uint16_t q = 0; q < tx_queues; q++) {
        struct rte_eth_txconf txconf;

        txconf                   = dev_info.default_txconf;
        txconf.offloads          = port_conf.txmode.offloads | RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP;
        txconf.tx_thresh.pthresh = 0;
        txconf.tx_thresh.wthresh = 0;
        txconf.tx_thresh.hthresh = 0;

        retval = rte_eth_tx_queue_setup(pid, q, nb_txd, socket_id, &txconf);
        if (retval < 0)
            goto err_exit;
    }

    // Get source MAC address
    if (rte_eth_macaddr_get(pid, &lport->src_mac) < 0) {
        printf("Can't get MAC address on port=%u : %s\n", pid, rte_strerror(rte_errno));
        goto err_exit;
    }

    // Convert the MAC address string into binary format
    char buff[64];
    rte_ether_unformat_addr(pinfo->dst_mac_str, &lport->dst_mac);

    rte_ether_format_addr(buff, sizeof(buff), &lport->dst_mac);
    printf("Port %u Dst MAC %s ", pid, buff);

    rte_ether_format_addr(buff, sizeof(buff), &lport->src_mac);
    printf("Src MAC %s\n", buff);

    if (rte_eth_dev_set_ptypes(pid, RTE_PTYPE_UNKNOWN, NULL, 0) < 0) {
        printf("Port %u, Failed to disable Ptype parsing\n", pid);
        goto err_exit;
    }

    /* Start the Ethernet port. */
    if ((retval = rte_eth_dev_start(pid)) < 0)
        goto err_exit;
    /* Enable RX in promiscuous mode for the Ethernet device. */
    if (_btst(PROMISCUOUS)) {
        retval = rte_eth_promiscuous_enable(pid);
        if (retval != 0) {
            printf("Promiscuous mode enable failed: %s\n", rte_strerror(-retval));
            goto err_exit;
        }
    }

    if (_btst(HW_RX_TIMESTAMP) || _btst(HW_TX_TIMESTAMP)) {
        if ((retval = rte_eth_timesync_enable(pid)) != 0) {
            printf("Warning: rte_eth_timesync_enable() failed: %s\n", rte_strerror(-retval));
            printf("Continuing without hardware timestamping\n");
            _bclr(HW_RX_TIMESTAMP);
            _bclr(HW_TX_TIMESTAMP);
        } else
            printf("Hardware timestamping enabled on port %u\n", pid);
    }

    return 0;
err_exit:
    rte_spinlock_unlock(&pinfo->port_lock);
    return -1;
}

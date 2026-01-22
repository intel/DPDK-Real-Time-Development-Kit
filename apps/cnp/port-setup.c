/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"

#include <rte_mbuf_dyn.h>

static int
__init_setup_defaults(lport_t *lport)
{
    memset(&lport->dev_info, 0, sizeof(struct rte_eth_dev_info));
    memset(&lport->port_conf, 0, sizeof(struct rte_eth_conf));
    lport->nb_rxd    = NUM_RX_DESC_DEFAULT;
    lport->nb_txd    = NUM_TX_DESC_DEFAULT;
    lport->rx_queues = 1;
    lport->tx_queues = 1;

    printf("Initializing lport %u:%u...\n", lport->pid, lport->qid);
    int sid = rte_eth_dev_socket_id(lport->pid);
    if (sid == SOCKET_ID_ANY)
        sid = 0;
    lport->sid = sid;
    printf("  Using socket ID %u for allocations\n", lport->sid);

    int retval = rte_eth_dev_info_get(lport->pid, &lport->dev_info);
    if (retval != 0) {
        printf("Error during getting device port %u info: %s\n", lport->pid, strerror(-retval));
        return -1;
    }
    return 0;
}

static int
__init_port_configure(lport_t *lport)
{
    printf("Port information:\n");

    lport->port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
    lport->port_conf.rx_adv_conf.rss_conf.rss_hf &= lport->dev_info.flow_type_rss_offloads;

    printf("  max_rx_pktlen: %u\n", lport->dev_info.max_rx_pktlen);
    printf("  max_rx_queues: %u, max_tx_queues: %u\n", lport->dev_info.max_rx_queues,
           lport->dev_info.max_tx_queues);
    if (lport->dev_info.max_rx_queues == 1)
        lport->port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    else
        lport->port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    lport->port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    // Adjust max_lro_pkt_size if needed
    if (lport->dev_info.max_lro_pkt_size > RTE_ETHER_MAX_LEN) {
        printf("  Adjusting max_lro_pkt_size from %u to %u\n", lport->dev_info.max_lro_pkt_size,
               RTE_ETHER_MAX_LEN);
        lport->port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;
    }
    printf("  max_lro_pkt_size: %u\n", lport->port_conf.rxmode.max_lro_pkt_size);
    printf("  Offload capabilities:\n");
    printf("    Max VFS: %u\n", lport->dev_info.max_vfs);
    if (lport->dev_info.max_vfs) {
        if (lport->port_conf.rx_adv_conf.rss_conf.rss_hf != 0) {
            printf("    Supports RSS with VMDQ, enable\n");
            lport->port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_VMDQ_RSS;
        }
    }

    printf("    Device flags: 0x%08x\n", *lport->dev_info.dev_flags);

    if (*lport->dev_info.dev_flags & RTE_ETH_DEV_INTR_LSC) {
        printf("    Supports Link Status Change interrupts\n");
        lport->port_conf.intr_conf.lsc = 1;
    }

    if (lport->dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        printf("    Supports TX mbuf fast free, enabled\n");
        lport->port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    if (lport->dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP) {
        if (_btst(HW_TIMESTAMP)) {
            printf("    Supports Rx/Tx hardware timestamping\n");
            lport->port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
        } else {
            printf("    Warning: Port %u does not support Rx/Tx hardware timestamping\n", lport->pid);
            _bclr(HW_TIMESTAMP);
        }
    } else {
        printf("    Warning: Port %u does not support Rx/Tx hardware timestamping\n", lport->pid);
        _bclr(HW_TIMESTAMP);
    }
    return 0;
}

static int
__init_device_configure(lport_t *lport)
{
    uint16_t tx_queues = lport->tx_queues;
    uint16_t rx_queues = lport->rx_queues;

    printf("  Number of RX/TX queues: %u/%u\n", rx_queues, tx_queues);

    /* Configure the Ethernet device. */
    if (rte_eth_dev_configure(lport->pid, rx_queues, tx_queues, &lport->port_conf) < 0)
        return -1;
    return 0;
}

static int
__init_adjust_rx_tx_desc(lport_t *lport)
{
    printf("  RX descriptor limits... min:%u max:%u align:%u\n", lport->dev_info.rx_desc_lim.nb_min,
           lport->dev_info.rx_desc_lim.nb_max, lport->dev_info.rx_desc_lim.nb_align);
    if (lport->dev_info.rx_desc_lim.nb_min > lport->nb_rxd)
        lport->nb_rxd = lport->dev_info.rx_desc_lim.nb_min;
    if (lport->dev_info.rx_desc_lim.nb_max > 0) {
        if (lport->nb_rxd > lport->dev_info.rx_desc_lim.nb_max)
            lport->nb_rxd = lport->dev_info.rx_desc_lim.nb_max;
    }
    printf("    Using %u RX descriptors\n", lport->nb_rxd);

    printf("  TX descriptor limits... min:%u max:%u align:%u\n", lport->dev_info.tx_desc_lim.nb_min,
           lport->dev_info.tx_desc_lim.nb_max, lport->dev_info.tx_desc_lim.nb_align);
    if (lport->dev_info.tx_desc_lim.nb_min > lport->nb_txd)
        lport->nb_txd = lport->dev_info.tx_desc_lim.nb_min;
    if (lport->dev_info.tx_desc_lim.nb_max > 0) {
        if (lport->nb_txd > lport->dev_info.tx_desc_lim.nb_max)
            lport->nb_txd = lport->dev_info.tx_desc_lim.nb_max;
    }
    printf("    Using %u TX descriptors\n", lport->nb_txd);

    if (rte_eth_dev_adjust_nb_rx_tx_desc(lport->pid, &lport->nb_rxd, &lport->nb_txd) != 0)
        return -1;

    return 0;
}

static int
__init_mempools(lport_t *lport)
{
    lport->rx_mp = lport_pktmbuf_pool("rx", lport->pid, lport->qid, DEFAULT_MBUF_COUNT,
                                      DEFAULT_MBUF_SIZE, DEFAULT_CACHE_SIZE);
    if (lport->rx_mp == NULL) {
        printf("Error during allocating mbuf pool for RX\n");
        return -1;
    }

    lport->tx_mp = lport_pktmbuf_pool("tx", lport->pid, lport->qid, DEFAULT_MBUF_COUNT,
                                      DEFAULT_MBUF_SIZE, DEFAULT_CACHE_SIZE);
    if (lport->tx_mp == NULL) {
        printf("Error during allocating mbuf pool for TX\n");
        return -1;
    }

    printf("  Rx Mempool: %s, count %u size %u\n", lport->rx_mp->name, lport->rx_mp->size,
           lport->rx_mp->elt_size);
    printf("  Tx Mempool: %s, count %u size %u\n", lport->tx_mp->name, lport->tx_mp->size,
           lport->tx_mp->elt_size);
    return 0;
}

static int
__init_rx_queues(lport_t *lport)
{
    /* Allocate and set up RX queue per Ethernet port. */
    for (uint16_t q = 0; q < lport->rx_queues; q++) {
        struct rte_eth_rxconf rxconf;

        memset(&rxconf, 0, sizeof(rxconf));
        rxconf                   = lport->dev_info.default_rxconf;
        rxconf.offloads          = lport->port_conf.rxmode.offloads;
        rxconf.rx_thresh.pthresh = 0;
        rxconf.rx_thresh.wthresh = 0;
        rxconf.rx_thresh.hthresh = 0;

        printf("  RX queue %2u setup... offloads 0x%08lx\n", q, rxconf.offloads);

        if (rte_eth_rx_queue_setup(lport->pid, q, lport->nb_rxd, lport->sid, &rxconf,
                                   lport->rx_mp) < 0)
            return -1;
    }
    return 0;
}

static int
__init_tx_queues(lport_t *lport)
{
    /* Allocate and set up TX queue per Ethernet port. */
    for (uint16_t q = 0; q < lport->tx_queues; q++) {
        struct rte_eth_txconf txconf;

        memset(&txconf, 0, sizeof(txconf));
        txconf                   = lport->dev_info.default_txconf;
        txconf.offloads          = lport->port_conf.txmode.offloads;
        txconf.tx_thresh.pthresh = 0;
        txconf.tx_thresh.wthresh = 0;
        txconf.tx_thresh.hthresh = 0;

        printf("  TX queue %2u setup... offloads 0x%08lx\n", q, txconf.offloads);

        if (rte_eth_tx_queue_setup(lport->pid, q, lport->nb_txd, lport->sid, &txconf) < 0)
            return -1;
    }
    return 0;
}

static int
__init_macaddr(lport_t *lport)
{
    uint16_t pid = lport->pid;

    // Get source MAC address
    if (rte_eth_macaddr_get(pid, &lport->src_mac) < 0) {
        printf("Can't get MAC address on port=%u : %s\n", pid, rte_strerror(rte_errno));
        return -1;
    }

    // Convert the MAC address string into binary format
    char buff[64];
    rte_ether_unformat_addr(pinfo->dst_mac_str, &lport->dst_mac);

    rte_ether_format_addr(buff, sizeof(buff), &lport->dst_mac);
    printf("Port %u Dst MAC %s ", pid, buff);

    rte_ether_format_addr(buff, sizeof(buff), &lport->src_mac);
    printf("Src MAC %s\n", buff);

    return 0;
}

static int
__init_set_ptypes(lport_t *lport)
{
    if (rte_eth_dev_set_ptypes(lport->pid, RTE_PTYPE_UNKNOWN, NULL, 0) < 0) {
        printf("Port %u, Failed to disable Ptype parsing\n", lport->pid);
        return -1;
    }
    return 0;
}

static int
__init_start_device(lport_t *lport)
{
    /* Start the Ethernet port. */
    return rte_eth_dev_start(lport->pid);
}

static int
__init_enable_timestamping(lport_t *lport)
{
    int retval;

    /* Enable timestamping AFTER starting the device (required for many NICs) */
    if (_btst(HW_TIMESTAMP)) {
        if ((retval = rte_eth_timesync_enable(lport->pid)) != 0) {
            printf("Warning: rte_eth_timesync_enable() failed: %s\n", rte_strerror(-retval));
            printf("Continuing without Rx/Tx hardware timestamping\n");
            _bclr(HW_TIMESTAMP);
        } else {
            printf("  Rx/Tx Hardware timestamping enabled on port %u\n", lport->pid);

            // Clear any stale TX timestamp from hardware
            struct timespec ts = {0};
            rte_eth_timesync_read_tx_timestamp(lport->pid, &ts);
        }
    }
    return 0;
}

static int
__init_promiscuous(lport_t *lport)
{
    /* Enable RX in promiscuous mode for the Ethernet device. */
    if (_btst(PROMISCUOUS)) {
        if (rte_eth_promiscuous_enable(lport->pid) < 0) {
            printf("Promiscuous mode enable failed on port %u\n", lport->pid);
            return -1;
        }
        printf("  Promiscuous mode enabled on port %u\n", lport->pid);
    }
    return 0;
}

static int
__init_timestamp_fields(lport_t *lport)
{
    int dynf = rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
    if (dynf >= 0)
        lport->rx_timestamp_flag = (1UL << dynf);

    dynf = rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
    if (dynf >= 0)
        lport->rx_timestamp_offset = dynf;

    printf("  RX timestamp dynfield offset: %u flag: 0x%016" PRIx64 "\n",
           lport->rx_timestamp_offset, lport->rx_timestamp_flag);
    return 0;
}

/*
 * Initializes a given port using global settings and default values.
 */
int
port_init(lport_t *lport)
{
    // clang-format off
    int (*inits[])(lport_t *) = {
		__init_setup_defaults,
		__init_port_configure,
		__init_device_configure,
		__init_adjust_rx_tx_desc,
		__init_mempools,
		__init_rx_queues,
		__init_tx_queues,
		__init_enable_timestamping,
		__init_macaddr,
		__init_set_ptypes,
		__init_start_device,
		__init_promiscuous,
		__init_timestamp_fields,
		NULL
	};
    // clang-format on

    if (!rte_eth_dev_is_valid_port(lport->pid))
        rte_exit(EXIT_FAILURE, "Invalid port %u\n", lport->pid);

    rte_spinlock_lock(&pinfo->port_lock);

    for (int i = 0; inits[i] != NULL; i++) {
        if (inits[i](lport) < 0) {
            rte_spinlock_unlock(&pinfo->port_lock);
            return -1;
        }
    }

    rte_spinlock_unlock(&pinfo->port_lock);
    return 0;
}

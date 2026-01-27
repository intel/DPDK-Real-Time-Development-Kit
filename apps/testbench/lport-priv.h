/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2025-2025 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_LPORT_PRIV_H
#define __INCLUDE_LPORT_PRIV_H

#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_time.h>

#include "config.h"
#include "security.h"
#include "stat.h"
#include "log.h"
#include "lport.h"

#define DEFAULT_QUEUE_COUNT 16

struct lport_tx_buffer_s {
    uint16_t size;   /**< Size of buffer for buffered Tx */
    uint16_t length; /**< Number of packets in the array */
    /** Pending packets to be sent on explicit flush or when full */
    struct rte_mbuf *pkts[];
};

typedef struct lqueue_s {
    lport_id_t id;                              // Port/Queue ID
    struct rte_mempool *rx_mp;                  // Rx mempool
    struct lport_tx_buffer_s *tx_buffer;        // Tx buffer queue
    struct rte_mempool *tx_mp;                  // Tx mempool
    uint64_t recv_timeout;                      // Receive Timeout counter
} lqueue_t;

typedef struct callback_s {
    void *cb;        // struct rte_eth_rxtx_callback *
    rte_rx_callback_fn fn;
} callback_t;

struct lport_s {
    uint16_t inited;                                 // structure is inited
    uint16_t pid;                                    // Port ID
    uint16_t sid;                                    // Socket ID for the port
    lqueue_t *lqueues;                               // Per Rx/Tx queue information
    lport_conf_t lpc;                                // Configuration for a port
    struct rte_eth_dev_info dev_info;                // device information
    struct rte_ether_addr mac;                       // MAC address for the port
    struct rte_eth_stats stats, prev;                // Port statistics and previous statistics
    uint16_t nb_cb;                                  // Number of registered callback functions
    callback_t callback[LPORT_MAX_CALLBACKS];        // Rx Callback information
};

typedef struct lport_info_s {
    volatile uint32_t link_running;          // Running flag
    lport_flags_e flags;                     // Flags for configuration
    lport_t lports[RTE_MAX_ETHPORTS];        // Set of lport structures
} lport_info_t __rte_cache_aligned;

extern lport_info_t *linfo;        // Global lport_info_t structure

/**
 * @brief Retrieve the queue associated with the specified logical port.
 *
 * This function retrieves the queue associated with the given logical port ID.
 * The queue is used for packet transmission and reception for the specified port.
 *
 * @param id The logical port ID (lport_id_t) for which to retrieve the queue.
 *
 * @return A pointer to the lqueue_t structure representing the queue associated with the specified
 * port. If the logical port ID is invalid or the queue cannot be found, the function returns NULL.
 */
lqueue_t *lport_queue_get(lport_id_t id);

static inline int
lport_tx_buffer_init(lport_tx_buffer_t *buffer, uint16_t size)
{
    int ret = 0;

    if (buffer == NULL) {
        fprintf(stderr, "Cannot initialize NULL buffer\n");
        return -EINVAL;
    }

    buffer->size = size;

    return ret;
}

static inline lport_tx_buffer_t *
lport_tx_buffer_alloc(lport_id_t id)
{
    lport_tx_buffer_t *tx_buffer;
    uint16_t pid = lport2pid(id);
    char buff[64];

    snprintf(buff, sizeof(buff), "txbuf-%s", lport_format(id));
    tx_buffer = (lport_tx_buffer_t *)rte_zmalloc_socket(
        buff, RTE_ETH_TX_BUFFER_SIZE(TX_BUFFER_SIZE), 0, rte_eth_dev_socket_id(pid));
    if (tx_buffer)
        lport_tx_buffer_init(tx_buffer, TX_BUFFER_SIZE);

    return tx_buffer;
}

/**
 * Send any packets queued up for transmission on a port and HW queue
 *
 * This causes an explicit flush of packets previously buffered via the
 * lport_tx_buffer() function. It returns the number of packets successfully
 * sent to the NIC. Unless
 *
 * @param id
 *   The lport identifier of the Ethernet device.
 * @return
 *   The number of packets successfully sent to the Ethernet device. The error
 *   callback is called for any packets which could not be sent.
 */
static inline uint16_t
lport_tx_buffer_flush(lport_id_t id)
{
    uint16_t pid = lport2pid(id), qid = lport2qid(id);
    lport_tx_buffer_t *buffer;
    struct rte_mbuf **pkts;
    uint16_t to_send;
    uint16_t sent = 0;

    buffer         = lport_queue_get(id)->tx_buffer;
    pkts           = buffer->pkts;
    to_send        = buffer->length;
    buffer->length = 0;
    for (int retries = 0; to_send > 0; ) {
        uint16_t n = rte_eth_tx_burst(pid, qid, pkts, to_send);
        to_send -= n;
        pkts += n;
        sent += n;
        if (n == 0) {
            if (++retries >= 100)
                break;
        } else {
            retries = 0;
        }
    }
    if (to_send > 0)
        rte_pktmbuf_free_bulk(pkts, to_send);

    return sent;
}

/**
 * Buffer a single packet for future transmission on a port and queue
 *
 * This function takes a single mbuf/packet and buffers it for later
 * transmission on the particular port and queue specified. Once the buffer is
 * full of packets, an attempt will be made to transmit all the buffered
 * packets. In case of error, where not all packets can be transmitted, a
 * callback is called with the unsent packets as a parameter. If no callback
 * is explicitly set up, the unsent packets are just freed back to the owning
 * mempool. The function returns the number of packets actually sent i.e.
 * 0 if no buffer flush occurred, otherwise the number of packets successfully
 * flushed
 *
 * @param id
 *   The lport identifier of the Ethernet device.
 * @param tx_pkt
 *   Pointer to the packet mbuf to be sent.
 * @return
 *   0 = packet has been buffered for later transmission
 *   N > 0 = packet has been buffered, and the buffer was subsequently flushed,
 *     causing N packets to be sent, and the error callback to be called for
 *     the rest.
 */
static __rte_always_inline uint16_t
lport_tx_buffer_add(lport_id_t id, struct rte_mbuf *tx_pkt)
{
    lport_tx_buffer_t *buffer;

    buffer                         = lport_queue_get(id)->tx_buffer;
    buffer->pkts[buffer->length++] = tx_pkt;
    if (buffer->length < buffer->size)
        return 0;

    return lport_tx_buffer_flush(id);
}

static __rte_always_inline uint16_t
lport_tx_buffer_count(lport_id_t id)
{
    return lport_queue_get(id)->tx_buffer->length;
}

static __rte_always_inline struct rte_mbuf *
lport_tx_buffer_get(lport_id_t id, uint16_t idx)
{
    return lport_queue_get(id)->tx_buffer->pkts[idx];
}

#endif /* __INCLUDE_LPORT_PRIV_H */

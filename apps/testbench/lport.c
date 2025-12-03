// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2025-2025 Intel Corporation. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/if_packet.h>

#include <rte_common.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_malloc.h>
#include <rte_hexdump.h>

#include "config.h"
#include "functions.h"
#include "lport.h"
#include "lport-priv.h"
#include "tx_time.h"
#include "utils.h"

#define RTE_LOGTYPE_LPORT  lport_logtype
#define LP_LOG(level, ...) RTE_LOG_LINE_PREFIX(level, LPORT, "%s(): ", __func__, __VA_ARGS__)

RTE_LOG_REGISTER_DEFAULT(lport_logtype, INFO);

static pthread_mutex_t lport_mutex = PTHREAD_MUTEX_INITIALIZER;        // Mutex for lport operations

#define LINK_TIMEOUT_SEC 30

static lport_info_t lport_info = {0};
lport_info_t *linfo            = &lport_info;

enum {
    RX_PTHRESH = 8, /**< Default values of RX prefetch threshold reg. */
    RX_HTHRESH = 8, /**< Default values of RX host threshold reg. */
    RX_WTHRESH = 4, /**< Default values of RX write-back threshold reg. */

    TX_PTHRESH     = 36, /**< Default values of TX prefetch threshold reg. */
    TX_HTHRESH     = 0,  /**< Default values of TX host threshold reg. */
    TX_WTHRESH     = 0,  /**< Default values of TX write-back threshold reg. */
    TX_WTHRESH_1GB = 16, /**< Default value for 1GB ports */
};

static struct rte_eth_conf default_port_conf = {
    .rxmode =
        {
            .mq_mode          = RTE_ETH_MQ_RX_NONE,
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

int
lport_link_running(void)
{
    return linfo->link_running;
}

void
lport_link_running_set(int val)
{
    linfo->link_running = val;
}

static inline void
lport_lock(void)
{
    int ret = pthread_mutex_lock(&lport_mutex);

    if (ret)
        fprintf(stderr, "failed: %s\n", strerror(ret));
}

static inline void
lport_unlock(void)
{
    int ret = pthread_mutex_unlock(&lport_mutex);

    if (ret)
        fprintf(stderr, "failed: %s\n", strerror(ret));
}

lport_t *
lport_get(lport_id_t id)
{
    uint16_t pid = lport2pid(id);

    if (pid >= RTE_MAX_ETHPORTS)
        rte_exit(EXIT_FAILURE, "%s: invalid port id %u\n", __func__, pid);

    return &linfo->lports[pid];
}

lqueue_t *
lport_queue_get(lport_id_t id)
{
    lport_t *lport = lport_get(id);
    uint16_t qid   = lport2qid(id);

    if (!lport)
        rte_exit(EXIT_FAILURE, "%s: pointer to lport is NULL\n", __func__);

    if (qid >= lport->lpc.nb_queues)
        fprintf(stderr, "%s: qid %u >= %u lport->lpc.nb_queues\n", __func__, qid,
                lport->lpc.nb_queues);
    return (qid >= lport->lpc.nb_queues) ? NULL : &lport->lqueues[qid];
}

uint64_t
lport_get_timeout(lport_id_t id)
{
    lport_t *lport = lport_get(id);
    uint16_t qid   = lport2qid(id);

    return lport->lqueues[qid].recv_timeout;
}

lport_t *
lport_alloc(lport_id_t id)
{
    lport_t *lport;
    uint16_t pid = lport2pid(id);
    char name[32];

    lport_lock();

    lport = &linfo->lports[pid];
    if (lport->inited == 0) {
        memset(lport, 0, sizeof(lport_t));
        lport->pid = pid;
        lport->sid = rte_eth_dev_socket_id(pid);

        if (lport->sid == (uint16_t)SOCKET_ID_ANY)
            lport->sid = 0;
        snprintf(name, sizeof(name) - 1, "lqueues-%d, sid %u", pid, lport->sid);
        lport->lqueues = rte_calloc_socket(name, DEFAULT_QUEUE_COUNT, sizeof(lqueue_t),
                                           RTE_CACHE_LINE_SIZE, lport->sid);
        if (!lport->lqueues) {
            lport_unlock();
            rte_exit(EXIT_FAILURE, "Unable to allocate lport queue structures\n");
        }
    }

    lport_unlock();

    return lport;
}

int
lport_free(lport_t *lport)
{
    if (lport) {
        lport_t *lp = &linfo->lports[lport->pid];

        lport_lock();
        if (lport != lp) {
            lport_unlock();
            fprintf(stderr, "%s: lport pointers don't match %p != %p\n", __func__, lport, lp);
            return -1;
        }

        memset(lport, 0, sizeof(lport_t));        // reset lport
        lport_unlock();
    }
    return 0;
}

struct rte_mempool *
lport_get_rx_mp(lport_id_t id)
{
    return lport_queue_get(id)->rx_mp;
}

struct rte_mempool *
lport_get_tx_mp(lport_id_t id)
{
    return lport_queue_get(id)->tx_mp;
}

uint16_t
lport_count(void)
{
    return rte_eth_dev_count_avail();
}

static struct rte_mempool *
lport_pktmbuf_pool(const char *name, lport_id_t id, uint32_t num_mbufs, uint32_t mbuf_size,
                   uint16_t cache_size)
{
    struct rte_mempool *mp;

    fprintf(stderr, "%s: lport %s: Entry\n", __func__, lport_format(id));

    fprintf(stderr, "%s: lport %s: Here 0 %s, num_mbufs %'u\n", __func__, lport_format(id), name,
            num_mbufs);
    mp = rte_pktmbuf_pool_create(name, num_mbufs, cache_size, DEFAULT_PRIV_SIZE, mbuf_size,
                                 rte_eth_dev_socket_id(lport2pid(id)));
    if (!mp)
        LP_LOG(ERR, "Failed to allocate mbuf pool %s: %s", name, rte_strerror(rte_errno));
    fprintf(stderr, "%s: lport %s: Exit\n", __func__, lport_format(id));

    return mp;
}

static int
lport_port_start(lport_id_t id)
{
    lport_t *lport = lport_get(id);

    if (rte_eth_dev_start(lport2pid(id)) < 0) {
        LP_LOG(ERR, "port=%d, %s", lport2pid(id), rte_strerror(-rte_errno));
        return -EINVAL;
    }

    if (lport->lpc.flags & LPORT_FLAG_PROMISCUOUS) {
        if (rte_eth_promiscuous_enable(lport2pid(id))) {
            LP_LOG(ERR, "Enabling promiscuous mode failed for port ID %u: %s", lport2pid(id),
                   rte_strerror(-rte_errno));
            return -EINVAL;
        }
    }

    return 0;
}

static int
lport_setup_queues(lport_id_t id)
{
    lport_t *lport = lport_get(id);
    uint16_t pid   = lport2pid(id);

    LP_LOG(INFO, "Setting up Rx/Tx queues for %s", lport_format(id));

    if (!lport)
        return -EINVAL;

    for (int q = 0; q < lport->lpc.nb_queues; q++) {
        struct rte_eth_rxconf rxq_conf;
        rxq_conf          = lport->dev_info.default_rxconf;
        rxq_conf.offloads = default_port_conf.rxmode.offloads;

        if (rte_eth_rx_queue_setup(pid, q, lport->lpc.nb_rxd, rte_eth_dev_socket_id(pid), &rxq_conf,
                                   lport_queue_get(id)->rx_mp) < 0) {
            LP_LOG(ERR, "Failed to setup RX queue for %s", lport_format(id));
            return -EINVAL;
        }

        struct rte_eth_txconf *txq_conf;
        txq_conf           = &lport->dev_info.default_txconf;
        txq_conf->offloads = default_port_conf.txmode.offloads;

        if (rte_eth_tx_queue_setup(pid, q, lport->lpc.nb_txd, rte_eth_dev_socket_id(pid),
                                   txq_conf) < 0) {
            LP_LOG(ERR, "Port %s, Failed to setup TX queue", lport_format(id));
            return -EINVAL;
        }
    }
    return 0;
}

static int
lport_port_init(lport_id_t id)
{
    lport_t *lport;
    lport_conf_t *lpc;
    struct rte_eth_conf conf = default_port_conf;
    uint16_t pid             = lport2pid(id);
    char buff[64];

    lport = lport_get(id);
    if (!lport) {
        LP_LOG(ERR, "lport ID %s not found", lport_format(id));
        return -1;
    }
    if (lport->inited)
        return 0;
    lpc = &lport->lpc;

    LP_LOG(INFO, "Initializing port with ID %u", pid);

    if (rte_eth_dev_info_get(lport2pid(id), &lport->dev_info)) {
        LP_LOG(ERR, "%s: failed to get device information", __func__);
        return -EINVAL;
    }

    conf.rx_adv_conf.rss_conf.rss_key = NULL;
    conf.rx_adv_conf.rss_conf.rss_hf &= lport->dev_info.flow_type_rss_offloads;

    if (lport->dev_info.max_vfs) {
        if (conf.rx_adv_conf.rss_conf.rss_hf != 0)
            conf.rxmode.mq_mode = RTE_ETH_MQ_RX_VMDQ_RSS;
    }

    conf.rxmode.offloads &= lport->dev_info.rx_offload_capa;

    if (app_config.application_link_speed != RTE_ETH_SPEED_NUM_UNKNOWN) {
        LP_LOG(INFO, "Link Speed and duplex: %'dMbps-%s", app_config.application_link_speed,
               app_config.application_link_half_duplex ? "HD" : "FD");
        conf.link_speeds = rte_eth_speed_bitflag(
            (app_config.application_link_speed == RTE_ETH_SPEED_NUM_UNKNOWN)
                ? RTE_ETH_LINK_SPEED_AUTONEG
                : app_config.application_link_speed,
            app_config.application_link_half_duplex ? RTE_ETH_LINK_HALF_DUPLEX
                                                    : RTE_ETH_LINK_FULL_DUPLEX);
    } else
        LP_LOG(INFO, "Link Speed and duplex: AutoNeg");

    LP_LOG(INFO, "Setup lport %s with %'u/%'u Rx/Tx queues", lport_format(id), lpc->nb_queues,
           lpc->nb_queues);
    if (rte_eth_dev_configure(pid, lpc->nb_queues, lpc->nb_queues, &conf) < 0) {
        LP_LOG(ERR, "Failed to configure device for port ID %u", pid);
        return -EINVAL;
    }

    LP_LOG(INFO, "Setup lport %s with %'u/%'u Rx/Tx descriptors", lport_format(id), lpc->nb_rxd,
           lpc->nb_txd);
    if (rte_eth_dev_adjust_nb_rx_tx_desc(pid, &lpc->nb_rxd, &lpc->nb_txd) < 0) {
        LP_LOG(ERR, "Failed to adjust number of RX/TX descriptors for port ID %u", pid);
        return -EINVAL;
    }

    if (rte_eth_macaddr_get(pid, &lport->mac) < 0) {
        LP_LOG(ERR, "Can't get MAC address: err=%s, port=%u", rte_strerror(-rte_errno), pid);
        return -EINVAL;
    }
    rte_ether_format_addr(buff, sizeof(buff) - 1, &lport->mac);
    LP_LOG(INFO, "Port %s MAC address: %s", lport_format(id), buff);

    if (rte_eth_dev_set_ptypes(pid, RTE_PTYPE_UNKNOWN, NULL, 0) < 0) {
        LP_LOG(ERR, "Port %u, Failed to disable Ptype parsing", pid);
        return -EINVAL;
    }

    if (lport_setup_queues(id))
        return -EINVAL;

    if (lport_port_start(id))
        return -EINVAL;

    lport->inited = 1;

    /* Return success */
    return 0;
}

static int
lqueue_setup(lport_t *lport, lport_id_t id, const char *name)
{
    lport_conf_t *lpc = &lport->lpc;
    lqueue_t *lqueue;
    uint32_t num_mbufs = 0;
    char buff[64];

    lqueue = lport_queue_get(id);
    if (lqueue == NULL) {
        LP_LOG(ERR, "%s: Failed to get lqueue for lport %s", __func__, lport_format(id));
        return -EINVAL;
    }

    num_mbufs = rte_align32pow2(lpc->nb_rx_mbufs);

    LP_LOG(INFO, "Setup lport %s with %'8d Rx mbufs, cache_size %'d", lport_format(id), num_mbufs,
           lpc->cache_sz);

    snprintf(buff, sizeof(buff) - 1, "%s-rx-%s", name, lport_format(id));
    fprintf(stderr, "%s: lport %s: Here 0 lqueue %p\n", __func__, lport_format(id), lqueue);
    lqueue->rx_mp = lport_pktmbuf_pool(buff, id, num_mbufs, lpc->mbuf_size, lpc->cache_sz);
    if (lqueue->rx_mp == NULL) {
        LP_LOG(ERR, "%s: lport_pktmbuf_pool(%s) failed", __func__, buff);
        goto err_exit;
    }

    fprintf(stderr, "%s: lport %s: Here 1\n", __func__, lport_format(id));
    if (lpc->nb_tx_mbufs) {
        num_mbufs = rte_align32pow2(lpc->nb_tx_mbufs);

        LP_LOG(INFO, "Setup lport %s with %'8d Tx mbufs, cache_size %'d", lport_format(id),
               num_mbufs, lpc->cache_sz);
        snprintf(buff, sizeof(buff) - 1, "%s-tx-%s", name, lport_format(id));
        fprintf(stderr, "%s: lport %s: Here 2 lqueue %p\n", __func__, lport_format(id), lqueue);

        lqueue->tx_mp = lport_pktmbuf_pool(buff, id, num_mbufs, lpc->mbuf_size, lpc->cache_sz);
        if (lqueue->tx_mp == NULL) {
            LP_LOG(ERR, "%s: lport_pktmbuf_pool_create(%s) failed", __func__, buff);
            goto err_exit;
        }
    }

    fprintf(stderr, "%s: lport %s: Here 2\n", __func__, lport_format(id));
    lqueue->tx_buffer = lport_tx_buffer_alloc(id);
    if (!lqueue->tx_buffer) {
        LP_LOG(ERR, "%s: Failed to allocate %s tx buffer!", __func__, name);
        goto err_exit;
    }

    fprintf(stderr, "%s: lport %s: setup done\n", __func__, lport_format(id));
    return 0;

err_exit:
    fprintf(stderr, "lport %s: Error Exit\n", lport_format(id));
    if (lqueue) {
        rte_mempool_free(lqueue->tx_mp);
        rte_mempool_free(lqueue->rx_mp);
    }
    return -1;
}

int
lport_setup(const char *name, lport_id_t id, lport_conf_t *cfg)
{
    lport_t *lport    = NULL;
    lport_conf_t *lpc = NULL;

    LP_LOG(INFO, "Setup %s thread for lport %s", name, lport_format(id));

    lport_lock();

    lport = lport_alloc(id);
    if (!lport) {
        LP_LOG(ERR, "%s: unable to allocate lport_t %s", __func__, lport_format(id));
        goto err_exit;
    }
    lpc = &lport->lpc;
    if (cfg)
        *lpc = *cfg;

    // Validate the port configuration
    if (lpc->nb_queues == 0)
        lpc->nb_queues = 1;        // Default to 1 RX queue if not specified

    if (lqueue_setup(lport, id, name) < 0)
        goto err_exit;

    if (lport_port_init(id))
        goto err_exit;

    lport_unlock();
    return 0;

err_exit:
    rte_free(lport->lqueues);
    lport_free(lport);
    lport_unlock();
    return -1;
}

int
lport_tx_get_bulk(lport_id_t id, struct rte_mbuf **mbufs, int nb_pkts)
{
    if (rte_mempool_get_bulk(lport_get_tx_mp(id), (void **)mbufs, nb_pkts)) {
        log_message(LOG_LEVEL_ERROR, "%s: lport_tx_get_bulk() failed\n", __func__);
        return -1;
    }

    return 0;
}

uint8_t *
lport_mac_address(lport_id_t id)
{
    lport_t *lport = lport_get(id);
    uint16_t pid   = lport2pid(id);

    if (lport == NULL)
        return NULL;

    if (!rte_eth_dev_is_valid_port(pid))
        rte_exit(EXIT_FAILURE, "Invalid port ID: %u lport: %s\n", pid, lport_format(id));

    if (lport->mac.addr_bytes[0] == 0) {
        int ret;

        if ((ret = rte_eth_macaddr_get(pid, &lport->mac)) < 0)
            rte_exit(EXIT_FAILURE, "Can't get MAC address: err=%s, port=%u\n", rte_strerror(-ret),
                     pid);
    }

    return lport->mac.addr_bytes;
}

int
lport_send(lport_id_t id)
{
    return lport_tx_buffer_flush(id);
}

static inline uint16_t
lport_rx(lport_id_t id, struct rte_mbuf **mbufs, int burst_sz, uint64_t timo_ns)
{
    uint16_t pid        = lport2pid(id);
    uint16_t qid        = lport2qid(id);
    uint16_t total_pkts = 0, to_recv = burst_sz;
    uint64_t curr_ns;

    timo_ns += clock_gettime_ns();
    while (to_recv > 0) {
        uint16_t nb_rx = rte_eth_rx_burst(pid, qid, mbufs, to_recv);
        if (nb_rx) {
            to_recv -= nb_rx;
            mbufs += nb_rx;
            total_pkts += nb_rx;
            continue;
        }
        curr_ns = clock_gettime_ns();
        if (curr_ns > timo_ns) {
            lport_queue_get(id)->recv_timeout++;
            break;
        }
    }

    return total_pkts;
}

int
lport_receive(lport_id_t id, struct rte_mbuf **mbufs, int burst_sz, uint64_t timo_ns)
{
    return lport_rx(id, mbufs, burst_sz, timo_ns);
}

uint16_t
lport_process_pkts(void *_ctx, int burst_sz, lport_id_t id,
                   void (*fn)(void *, struct rte_mbuf **, uint16_t nb_pkts), uint64_t timo)
{
    struct thread_context *ctx = _ctx;
    struct rte_mbuf **mbufs    = ctx->mbufs;
    uint16_t nb_pkts;

    if ((nb_pkts = lport_receive(id, mbufs, burst_sz, timo)) > 0)
        fn(ctx, mbufs, nb_pkts);

    return nb_pkts;
}

int
lport_add_rx_callback(lport_id_t id, rte_rx_callback_fn fn, void *arg)
{
    lport_t *lport = lport_get(id);
    const struct rte_eth_rxtx_callback *cb;
    uint16_t pid, qid;

    if (!lport)
        return -EINVAL;

    pid = lport2pid(id);
    qid = lport2qid(id);

    fprintf(stderr, "%s: Install RX Callback for %u/%u\n", __func__, pid, qid);
    cb = rte_eth_add_rx_callback(pid, qid, fn, arg);
    if (!cb)
        return -EINVAL;

    for (uint32_t i = 0; i < LPORT_MAX_CALLBACKS; i++) {
        callback_t *c = &lport->callback[i];

        if (rte_atomic64_cmpset((volatile uint64_t *)&c->cb, 0, (uint64_t)cb)) {
            c->fn = fn;
            lport->nb_cb++;
            fprintf(stderr, "%s: Install Done RX Callback for %u/%u\n", __func__, pid, qid);
            return 0;
        }
    }
    if (cb)
        rte_eth_remove_rx_callback(pid, qid, cb);
    return -ENOSPC;
}

int
lport_remove_rx_callback(lport_id_t id, rte_rx_callback_fn fn)
{
    lport_t *lport = lport_get(id);
    const struct rte_eth_rxtx_callback *cb;
    uint16_t pid, qid;

    if (!lport)
        return -EINVAL;

    pid = lport2pid(id);
    qid = lport2qid(id);

    for (uint32_t i = 0; i < LPORT_MAX_CALLBACKS; i++) {
        callback_t *c = &lport->callback[i];

        if ((cb = c->cb) && c->fn == fn) {
            c->cb = NULL;
            c->fn = NULL;
            lport->nb_cb--;
            return rte_eth_remove_rx_callback(pid, qid, cb);
        }
    }
    return 0;
}

bool
lport_link_status_wait(lport_id_t id, struct rte_eth_link *link, int sec)
{
    struct rte_eth_link lk = {0};

    if (!link)
        link = &lk;

    lport_link_running_set(1);
    sec *= 4;
    while (sec && !lport_link_get(id, link)) {
        if (lport_link_running() == 0)
            break;
        usleep(USEC_PER_SEC / 4);
        sec--;
    }
    lport_link_running_set(0);

    return (link->link_status == RTE_ETH_LINK_UP);
}

static int
lport_ctor(void)
{
    memset(&lport_info, 0, sizeof(lport_info));
    linfo = &lport_info;

    fprintf(stderr, "Found %'d available Ethernet ports, number of lcores %'d\n",
            rte_eth_dev_count_avail(), rte_lcore_count());

    return 0;
}

static void
lport_dtor(void)
{
    memset(&lport_info, 0, sizeof(lport_info));
}

static int
lport_init(void *arg __rte_unused)
{
    if (mutex_create(&lport_mutex, PTHREAD_MUTEX_RECURSIVE_NP))
        rte_exit(EXIT_FAILURE, "failed to create lport mutex\n");

    if (lport_ctor())
        return -1;

    return 0;
}

static int
lport_launch(void *arg __rte_unused)
{
    return 0;
}

static void
lport_deinit(void *arg __rte_unused)
{
    lport_dtor();

    if (mutex_destroy(&lport_mutex))
        rte_exit(EXIT_FAILURE, "failed to destroy lport mutex\n");
}

FUNCTION_REGISTER(lport, LPORT_IDX);

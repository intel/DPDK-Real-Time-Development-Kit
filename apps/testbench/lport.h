/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2025-2025 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_LPORT_H
#define __INCLUDE_LPORT_H

#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_time.h>

#include "security.h"
#include "stat.h"

enum {
    MAX_PKT_BURST                 = 256,                            // Max burst size
    TX_BUFFER_SIZE                = (MAX_PKT_BURST * 4),            // Tx buffer size
    MEMPOOL_CACHE_SIZE            = 128,                            // Mempool cache size
    USEC_PER_SEC                  = (NSEC_PER_SEC / 1000UL),        // micro-seconds per second
    LPORT_INVALID                 = 0xFFFFFFFF,                     // LPort invalid value
    LPORT_VIRTUAL_PID             = 0xFFFF,        // Port ID used to denote a virtual port
    DEFAULT_PRIV_SIZE             = 0,             // Default private size for mempool
    NAME_SIZE                     = 32,            // Max name string size
    MAX_FRAME_SIZE                = 2048,          // Max frame size
    MAX_NUM_LPORTS                = 16,            // Max number of LPorts
    LPORT_MAX_VLAN_IDS            = 4096,          // 0 is reserved
    LPORT_PROFINET_RT             = 0x8892,        // PROFINET Realtime Ethernet type
    LPORT_MAX_NUM_QUEUES          = 16,            // Per Port ID
    LPORT_MAX_CALLBACKS           = 8,             // Max number of callbacks
    MIN_LINK_STRING_SIZE          = 24,            // Min number of bytes
    LPORT_PKTMBUF_FREE_PENDING_SZ = 64,            // Size of dropped mbuf list
};

typedef uint32_t lport_id_t;        // logical port using port ID and queue ID type

typedef struct lport_gen_config_s {
    enum security_mode mode;                          // security mode
    struct security_context *security_context;        // security context
    const unsigned char *iv_prefix;                   // IV prefix
    const unsigned char *payload_pattern;             // payload pattern
    size_t payload_pattern_length;                    // payload pattern length
    size_t frame_length;                              // frame length
    size_t num_frames_per_cycle;                      // number of frames per cycle
    uint64_t sequence_counter_begin;                  // sequence counter begin
    uint32_t meta_data_offset;                        // meta data offset
    enum stat_frame_type frame_type;                  // frame type
    const unsigned char *source;                      // source MAC address
    const unsigned char *destination;                 // destination MAC address
    void (*init_frame)(void *, unsigned char *, unsigned int, const unsigned char *,
                       const unsigned char *);        // initialize frame function
    void *init_arg;                                   // initialization argument
    uint64_t wakeup_time;                             // wakeup time
    uint64_t duration;                                // duration
} lport_gen_config_t;

typedef void(lport_setup_fn)(void *arg);

enum {
    PROMISCUOUS_IDX = 0,        // Enable promiscuous mode for the port
    JUMBO_FRAMES_IDX,           // Enable jumbo frames support
    LINK_STATUS_CHANGE,         // Enable Link Status Change interrupt
    DISABLE_RSS_IDX,            // Disable RSS support
    LPORT_FLAG_COUNT,           // Max number of flags
};

typedef enum {
    LPORT_FLAG_PROMISCUOUS  = (1 << PROMISCUOUS_IDX),           // Enable promiscuous mode
    LPORT_FLAG_JUMBO_FRAMES = (1 << JUMBO_FRAMES_IDX),          // Enable jumbo frames support
    LPORT_FLAG_LSC          = (1 << LINK_STATUS_CHANGE),        // Link Status Change
    LPORT_FLAG_DISABLE_RSS  = (1 << DISABLE_RSS_IDX),           // Disable RSS support
} lport_flags_e;

#define LPORT_ALL_FLAGS \
    (LPORT_FLAG_PROMISCUOUS | LPORT_FLAG_JUMBO_FRAMES | LPORT_FLAG_LSC | LPORT_FLAG_DISABLE_RSS)

#define LPORT_FLAG_STR {"Promiscuous", "JumboFrames", "TxBuffer", "LSC", "TxMbufs", "NoRSS"}

typedef struct lport_conf_s {
    lport_flags_e flags;         // Flags for port configuration, see above lport_flags_e
    uint16_t pid;                // Port ID value
    uint16_t cache_sz;           // Cache size for the port
    uint16_t nb_queues;          // Number of receive/transmit queues
    uint16_t nb_rxd;             // Number of receive descriptors
    uint16_t nb_txd;             // Number of transmit descriptors
    uint16_t mbuf_size;          // Size of each mbuf
    uint16_t reserved;           // Reserved for future use
    uint32_t nb_rx_mbufs;        // Number of mbufs in the Rx pool
    uint32_t nb_tx_mbufs;        // Number of mbufs in the Tx pool
} lport_conf_t;

struct lport_s;
struct lport_entry_s;
struct lport_tx_buffer_s;

typedef struct lport_s lport_t;
typedef struct lport_entry_s lport_entry_t;
typedef struct lport_tx_buffer_s lport_tx_buffer_t;

int lport_link_running(void);
void lport_link_running_set(int val);

lport_t *lport_get(lport_id_t id);
lport_t *lport_alloc(lport_id_t id);
int lport_free(lport_t *lport);

lport_entry_t *lport_entry_alloc(lport_id_t id, lport_t *lport);
int lport_entry_free(lport_entry_t *entry);

int lport_setup(const char *name, lport_id_t id, lport_conf_t *cfg);

uint16_t lport_count(void);
bool lport_link_status_wait(lport_id_t id, struct rte_eth_link *link, int sec);

struct rte_mempool *lport_get_rx_mp(lport_id_t id);
struct rte_mempool *lport_get_tx_mp(lport_id_t id);

int lport_tx_get_bulk(lport_id_t id, struct rte_mbuf **mbufs, int nb_pkts);

int lport_receive(lport_id_t id, struct rte_mbuf **mbufs, int burst_sz, uint64_t timo_ns);

int lport_send(lport_id_t id);

uint16_t lport_process_pkts(void *ctx, int burst_sz, lport_id_t id,
                            void (*fn)(void *, struct rte_mbuf **, uint16_t nb_pkts),
                            uint64_t timo);

uint8_t *lport_mac_address(lport_id_t id);

int lport_add_rx_callback(lport_id_t id, rte_rx_callback_fn fn, void *arg);

int lport_remove_rx_callback(lport_id_t id, rte_rx_callback_fn fn);

uint64_t lport_get_timeout(lport_id_t id);

static inline uint16_t
lport2pid(lport_id_t id)
{
    return (id & 0xFFFF0000) >> 16;
}

static inline uint16_t
lport2qid(lport_id_t id)
{
    return id & 0xFFFF;
}

static inline lport_id_t
lport_make(uint16_t pid, uint16_t qid)
{
    return (pid << 16) | qid;
}

static inline char *
lport_format(lport_id_t id)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%u:%u", lport2pid(id), lport2qid(id));
    return buf;
}

static inline bool
lport_link_get(lport_id_t id, struct rte_eth_link *link)
{
    struct rte_eth_link lk = {0};

    if (link == NULL)
        link = &lk;

    if (lport2pid(id) >= RTE_MAX_ETHPORTS)
        rte_exit(EXIT_FAILURE, "Invalid port ID: %u lport: %s\n", lport2pid(id), lport_format(id));

    if (rte_eth_link_get_nowait(lport2pid(id), link) != 0)
        rte_exit(EXIT_FAILURE, "get link status failed for %d\n", lport2pid(id));

    return (link->link_status == RTE_ETH_LINK_UP);
}

static inline bool
is_link_up(lport_id_t id)
{
    return lport_link_get(id, NULL);
}

static inline const char *
lport_link_string(lport_id_t lport, char *buff, uint32_t len)
{
    struct rte_eth_link link = {0};

    if (buff == NULL || len < MIN_LINK_STRING_SIZE)
        return NULL;

    if (lport2pid(lport) >= RTE_MAX_ETHPORTS)
        rte_exit(EXIT_FAILURE, "Invalid port ID: %u lport: %s\n", lport2pid(lport),
                 lport_format(lport));

    if (!lport_link_get(lport, &link))
        snprintf(buff, len - 1, "<Down>");
    else
        snprintf(buff, len - 1, "<UP-%'d-%s>", link.link_speed,
                 (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "FD" : "HD");
    return buff;
}

static inline void
lport_set_defaults(lport_id_t id, lport_conf_t *lpc)
{
    memset(lpc, 0, sizeof(lport_conf_t));
    lpc->pid         = lport2pid(id);
    lpc->cache_sz    = app_config.application_cache_size;
    lpc->nb_queues   = app_config.qinfo[lport2pid(id)].cnt;
    lpc->nb_rxd      = app_config.application_num_rx_descriptors;
    lpc->nb_txd      = app_config.application_num_tx_descriptors;
    lpc->mbuf_size   = app_config.application_mbuf_size;
    lpc->nb_rx_mbufs = app_config.application_num_mbufs;
}

#endif /* __INCLUDE_LPORT_H */

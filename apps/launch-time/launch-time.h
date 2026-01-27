/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef _LAUNCH_TIME_H_
#define _LAUNCH_TIME_H_

#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <pthread.h>
#ifndef DISABLE_MQTT
#include <mosquitto.h>
#endif

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_common.h>
#include <rte_malloc.h>

#include "consts.h"
#include "stats.h"
#include "mqtt.h"

typedef struct lport {
    struct rte_ether_addr src_mac;          // Source MAC address
    struct rte_ether_addr dst_mac;          // Destination MAC address
    struct rte_mempool *rx_mp;              // Memory pool for Rx mbufs
    struct rte_mempool *tx_mp;              // Memory pool for Tx mbufs
    uint16_t lcore_id;                      // Lcore ID
    uint16_t pid, qid;                      // Port ID and Queue ID, assume 0,0 for now
    struct rte_eth_link link;               // Link status
    struct rte_eth_stats stats;             // Port Statistics
    struct rte_eth_stats stats_prev;        // Previous port statistics
} lport_t;

typedef struct lcore {
    lport_t lport;                     // Port associated with the lcore
    stats_t stats;                     // Statistics for the lcore
    struct rte_mbuf **rx_mbufs;        // Array of mbufs for packet receive
    struct rte_mbuf **tx_mbufs;        // Array of mbufs for packet transmit
} lcore_t;

typedef struct {
    rte_atomic32_t flags;                 // Flags for internal use
    uint32_t reserved;                    // reserved for future use
    lcore_t lcores[RTE_MAX_LCORE];        // Array of lcore structures
    uint64_t launch_interval_ns;          // Launch interval in nanoseconds
    uint16_t mqtt_lcore_id;               // MQTT lcore ID
    uint16_t worker_lcore_id;             // Worker lcore ID
    uint16_t delay_sec;                   // Start delay in seconds
    uint16_t burst_count;                 // Burst count for packet transmission
    uint16_t pkt_length;                  // Length of packet minus the FCS
    uint16_t pad0;                        // Padding for alignment
    uint32_t link_speed;                  // Link speed in Mbps
    uint32_t run_duration_sec;            // Run duration in seconds
    uint32_t tx_burst_offset_ns;          // TX burst offset in nanoseconds before cycle end
    char *burst_length_str;               // Burst/Length string 'Burst/Length'
    char *run_duration_str;               // Run duration string
    char *dest_mac_str;                   // Destination MAC address in string format
    uint64_t run_duration_end_ns;         // Run duration end time in nanoseconds
    uint64_t rx_timestamp_flag;           // mbuf Rx timestamp flag
    uint64_t tx_timestamp_flag;           // mbuf Tx timestamp flag
    int rx_timestamp_offset;              // Rx Timestamp offset
    int tx_timestamp_offset;              // Tx Timestamp offset
    struct timespec start_time;           // Start time of the application
    struct termios oldterm;               // Old terminal setup information
} info_t;

extern info_t *pinfo;
typedef int (*timestamping_fn)(lcore_t *lcore, uint16_t pid, uint16_t qid);

enum {                             // Bit values for info_t.flags field
    APP_RUNNING_FLAG = 0,          // Main Running flag
    TTY_IS_INITED_FLAG,            // TTY has been inited flag
    LOG_FLAG,                      // Logging is enabled flag
    MQTT_FLAG,                     // MQTT Logging is enabled flag
    LAUNCH_TIME_FLAG,              // Launch time support enabled flag
    PROMISCUOUS_FLAG,              // Port promiscuous enabled flag
    HW_TIMESTAMP_FLAG,             // Hardware timestamping is enabled flag
    CLEAR_SCREEN_FLAG,             // Clear the screen flag
    RESET_STATS_FLAG,              // Clear the statistics flag
};

#define _bset(name)                                                                     \
    do {                                                                                \
        uint32_t _e, _s;                                                                \
        do {                                                                            \
            _e = rte_atomic32_read(&pinfo->flags);                                      \
            _s = (_e | (1 << (uint32_t)name##_FLAG));                                   \
        } while (rte_atomic32_cmpset((volatile uint32_t *)&pinfo->flags, _e, _s) == 0); \
    } while (/*CONSTCOND*/ 0)

#define _bclr(name)                                                                     \
    do {                                                                                \
        uint32_t _e, _s;                                                                \
        do {                                                                            \
            _e = rte_atomic32_read(&pinfo->flags);                                      \
            _s = (_e & ~(1 << (uint32_t)name##_FLAG));                                  \
        } while (rte_atomic32_cmpset((volatile uint32_t *)&pinfo->flags, _e, _s) == 0); \
    } while (/*CONSTCOND*/ 0)

#define _btst(name) (rte_atomic32_read(&pinfo->flags) & (1 << (uint32_t)name##_FLAG))

static inline void
stdin_restore(void)
{
    if (_btst(TTY_IS_INITED) && tcsetattr(0, TCSANOW, &pinfo->oldterm))
        fprintf(stderr, "%s: failed to set tty\n", __func__);
}

static inline uint64_t
ts_to_ns(const struct timespec *ts)
{
    return ((uint64_t)ts->tv_sec * NSEC_PER_SEC) + ts->tv_nsec;
}

static inline uint64_t
clock_get_ns(void)
{
    struct timespec ctime = {0};

    if (clock_gettime(CLOCK_TAI, &ctime) < 0)
        return 0;
    return ts_to_ns(&ctime);
}

static inline uint64_t
port_clock_get_ns(uint16_t pid)
{
    uint64_t clock = 0;

    if (rte_eth_read_clock(pid, &clock) < 0)
        return 0;

    return clock;
}

static inline void
start_running(void)
{
    _bset(APP_RUNNING);
}

static inline void
stop_running(void)
{
    _bclr(APP_RUNNING);
}

static inline bool
is_running(void)
{
    if (!_btst(APP_RUNNING) ||
        (pinfo->run_duration_end_ns && clock_get_ns() >= pinfo->run_duration_end_ns))
        return false;
    else
        return true;
}

static inline void
increment_period(struct timespec *time, int64_t period_ns)
{
    time->tv_nsec += period_ns;

    while (time->tv_nsec >= NSEC_PER_SEC) {
        /* timespec nsec overflow */
        time->tv_sec++;
        time->tv_nsec -= NSEC_PER_SEC;
    }
}

static inline void
sleep_sec(uint32_t sec)
{
    if (sec > 0) {
        struct timespec wakeup_time;

        clock_gettime(CLOCK_TAI, &wakeup_time);
        wakeup_time.tv_sec += sec;
        clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
    }
}

static inline void
sleep_usec(uint32_t usec)
{
    if (usec > 0) {
        struct timespec wakeup_time;

        clock_gettime(CLOCK_TAI, &wakeup_time);
        wakeup_time.tv_nsec += (uint64_t)(usec * 1000UL);
        while (wakeup_time.tv_nsec >= NSEC_PER_SEC) {
            wakeup_time.tv_nsec -= NSEC_PER_SEC;
            wakeup_time.tv_sec++;
        }
        clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
    }
}

static inline void
sleep_msec(uint32_t msec)
{
    sleep_usec(msec * 1000UL);
}

static inline void
sleep_nsec(uint64_t nsec)
{
    if (nsec > 0) {
        struct timespec wakeup_time;

        clock_gettime(CLOCK_TAI, &wakeup_time);
        wakeup_time.tv_nsec += nsec;
        while (wakeup_time.tv_nsec >= NSEC_PER_SEC) {
            wakeup_time.tv_nsec -= NSEC_PER_SEC;
            wakeup_time.tv_sec++;
        }
        clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
    }
}

static inline void
link_status_no_wait(lport_t *lport, char *buff, int len)
{
    struct rte_eth_link *link = &lport->link;
    uint32_t link_speed       = lport->link.link_speed;

    if (rte_eth_link_get_nowait(lport->pid, link) == 0) {
        if (link->link_speed == RTE_ETH_SPEED_NUM_UNKNOWN) {
            /* Setup a few default values to prevent problems later. */
            link->link_speed   = RTE_ETH_SPEED_NUM_10G;
            link->link_duplex  = RTE_ETH_LINK_FULL_DUPLEX;
            link->link_autoneg = RTE_ETH_LINK_SPEED_AUTONEG;
            link->link_status  = RTE_ETH_LINK_UP;
        }
        if (link->link_status == RTE_ETH_LINK_UP && link_speed != link->link_speed)
            lport->link = *link;        // structure copy
    }
    if (link->link_status == RTE_ETH_LINK_DOWN) {
        if (buff && len > 0)
            snprintf(buff, len, "<Down>");
    } else {
        if (buff && len > 0)
            snprintf(buff, len, "<UP-%'u-%s>", link->link_speed,
                     (link->link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "FD" : "HD");
    }
}

static inline bool
is_link_up(uint16_t pid)
{
    struct rte_eth_link link = {0};

    if (rte_eth_link_get_nowait(pid, &link) < 0)
        return 0;
    return (link.link_status == RTE_ETH_LINK_UP);
}

static inline void
send_packets(uint16_t pid, uint16_t qid, struct rte_mbuf **mbufs, uint16_t num_mbufs)
{
    uint16_t nb_tx;

    do {
        nb_tx = rte_eth_tx_burst(pid, qid, mbufs, num_mbufs);
        num_mbufs -= nb_tx;
        if (num_mbufs == 0)
            break;
        mbufs += nb_tx;
    } while (is_running());

    if (num_mbufs > 0)
        rte_pktmbuf_free_bulk(mbufs, num_mbufs);
}

static inline struct rte_mempool *
lport_pktmbuf_pool(const char *name, uint16_t pid, uint16_t qid, uint32_t num_mbufs,
                   uint32_t mbuf_size, uint16_t cache_size)
{
    struct rte_mempool *mp;
    char buff[RTE_MEMPOOL_NAMESIZE] = {0};

    snprintf(buff, sizeof(buff), "%s-%d/%d", name, pid, qid);
    mp = rte_pktmbuf_pool_create(buff, num_mbufs, cache_size, DEFAULT_PRIV_SIZE, mbuf_size,
                                 rte_eth_dev_socket_id(pid));
    if (!mp)
        rte_exit(EXIT_FAILURE, "Failed to allocate mbuf pool %s: %s\n", buff,
                 rte_strerror(rte_errno));
    return mp;
}

static inline void
min_avg_max_update(min_avg_max_t *mma, uint64_t ns)
{
    if (ns < mma->min_ns)
        mma->min_ns = ns;
    if (ns > mma->max_ns)
        mma->max_ns = ns;
    mma->sum_ns += ns;
    mma->count++;
}

int parse_args(int argc, char **argv);
void print_app_usage(const char *prgname);
int port_init(lport_t *lport);
void keyboard_loop(void);
int rxtx_routine(void *arg);

#endif

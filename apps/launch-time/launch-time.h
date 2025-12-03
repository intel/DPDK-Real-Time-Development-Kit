/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#ifndef _RTC_M2_H_
#define _RTC_M2_H_

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
#include <mosquitto.h>

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
    uint64_t cycle_time_ns;               // Cycle time in nanoseconds
    uint16_t mqtt_lcore_id;               // MQTT lcore ID
    uint16_t worker_lcore_id;             // Worker lcore IDF
    uint16_t delay_sec;                   // Start delay in seconds
    uint16_t burst_count;                 // Burst count for packet transmission
    uint16_t pkt_length;                  // Length of packet minus the FCS
    uint16_t pad0;                        // Padding for alignment
    uint32_t link_speed;                  // Link speed in Mbps
    uint32_t run_duration_sec;            // Run duration in seconds
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
#define TX_BURST_TIME_NS	60000    // Number ns (60us) to reduce from cycle time to account for TX processing time

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
        wakeup_time.tv_nsec += (uint64_t)(usec * USEC_PER_SEC);
        if (wakeup_time.tv_nsec > NSEC_PER_SEC) {
            wakeup_time.tv_nsec -= NSEC_PER_SEC;
            wakeup_time.tv_sec++;
        }
        clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
    }
}

static inline void
sleep_nsec(uint64_t nsec)
{
    if (nsec > 0) {
        struct timespec wakeup_time;

        clock_gettime(CLOCK_TAI, &wakeup_time);
        wakeup_time.tv_nsec += nsec;
        if (wakeup_time.tv_nsec > NSEC_PER_SEC) {
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
            snprintf(buff, len, "<UP-%'d-%s>", link->link_speed,
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
    lcore_t *lcore = (lcore_t *)&pinfo->lcores[rte_lcore_id()];
    uint16_t nb_tx;

	(void)lcore;

    do {
        nb_tx = rte_eth_tx_burst(pid, qid, mbufs, num_mbufs);
        num_mbufs -= nb_tx;
        if (num_mbufs == 0)
            break;
        mbufs += nb_tx;
    } while (is_running());
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
    else if (ns > mma->max_ns)
        mma->max_ns = ns;
    mma->sum_ns += ns;
    mma->count++;
}

static inline void
begin_time(uint64_t *begin)
{
    *begin = clock_get_ns();
}

static inline void
end_time(min_avg_max_t *mma, uint64_t begin)
{
    min_avg_max_update(mma, clock_get_ns() - begin);
}

static inline void
print_clock(const char *s, const char *s1, uint64_t curr, const char *s2, uint64_t end_cycle)
{
    printf("INFO: %s %5s: %'" PRIu64 " >  %5s: %'" PRIu64, s, s1, curr, s2, end_cycle);
    printf(" Delta:%'8" PRIu64 "\n", (curr > end_cycle) ? curr - end_cycle : end_cycle - curr);
}

/// Round up to next higher power of 2 (return x if it's already a power of 2).
static inline int
pow2roundup(u_int32_t x)
{
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

int parse_args(int argc, char **argv);
void print_app_usage(const char *prgname);
int port_init(lport_t *lport);
void keyboard_loop(void);
int rxtx_routine(void *arg);

#endif

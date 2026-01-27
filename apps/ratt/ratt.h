/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef _RATT_H_
#define _RATT_H_

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
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

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

#define WORKLOAD_MAX_ARGS 1024

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
    rte_atomic16_t active;             // Flag to indicate if the lcore is active
    lport_t lport;                     // Port associated with the lcore
    uint64_t end_cycle_ns;             // End cycle time in nanoseconds
    uint16_t tx_id;                    // Transmit ID
    uint16_t rx_id;                    // Receive ID
    uint32_t skip_cnt;                 // Number of packets to skip on startup
    stats_t stats;                     // Statistics for the lcore
    struct rte_mbuf **rx_mbufs;        // Array of mbufs for packet receive
    struct rte_mbuf **tx_mbufs;        // Array of mbufs for packet transmit
} lcore_t;

typedef struct workload {
    bool enabled;
    char *file;
    char *func;
    char args[WORKLOAD_MAX_ARGS];
    void *workload_handler;
    int (*workload_function)(int argc, char **argv);
    int workload_argc;
    char *workload_argv[WORKLOAD_MAX_ARGS];
} workload_t;

typedef struct {
    lcore_t lcores[RTE_MAX_LCORE];        // Array of lcore structures
    uint64_t cycle_time_ns;               // Cycle time in nanoseconds
    uint16_t mqtt_lcore_id;               // MQTT lcore ID
    uint16_t worker_lcore_id;             // Worker lcore IDF
    uint16_t delay_sec;                   // Start delay in seconds
    uint16_t burst_count;                 // Burst count for packet transmission
    uint16_t pkt_length;                  // Length of packet minus the FCS
    uint16_t pad0;                        // Padding for alignment
    uint32_t link_speed;                  // Link speed in Mbps
    uint32_t pkt_skip_cnt;                // Number of packets to skip on startup
    uint32_t run_duration_sec;            // Run duration in seconds
    char *burst_length_str;               // Burst/Length string 'Burst/Length'
    char *run_duration_str;               // Run duration string
    char *dest_mac_str;                   // Destination MAC address in string format
    uint64_t run_duration_end_ns;         // Run duration end time in nanoseconds
    struct timespec start_time;           // Start time of the application
    bool mirror_enabled;                  // Mirror enabled (Default: false=reference enabled)
    bool running;                         // Flag to indicate if the application is running
    bool screen_clear;                    // Flag to indicate if screen should be cleared
    bool reset_stats;                     // Flag to indicate if statistics should be reset
    bool log_enabled;                     // Flag to indicate if log messages are enabled
    bool mqtt_enabled;                    // Flag to indicate if MQTT messages are enabled
    bool deltas_enabled;                  // Flag to indicate if all time deltas are logged
    bool mirror_serial_enabled;           // Mirror to serial port (Default: false=disabled)
    bool internal_debug_enabled;          // Enable internal debugging statistics
    bool promiscuous_mode;                // Promiscuous mode
#if HAS_HW_TIMESTAMPING
    bool hw_timestamp_enabled;        // Enable hardware timestamp
#endif
    bool continue_on_error;        // Continue on error
    bool tty_inited;               // Flag to indicate if terminal setup was done
    struct termios oldterm;        // Old terminal setup information
    workload_t rt_workload;
} info_t;

extern info_t *pinfo;

static inline void
stdin_restore(void)
{
    if (pinfo->tty_inited && tcsetattr(0, TCSANOW, &pinfo->oldterm))
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

static inline void
stop_running(void)
{
    pinfo->running = false;
}

static inline bool
is_running(void)
{
    if (pinfo->running == false ||
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

static inline int
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

    do {
        nb_tx = rte_eth_tx_burst(pid, qid, mbufs, num_mbufs);
        num_mbufs -= nb_tx;
        if (num_mbufs == 0)
            break;
        mbufs += nb_tx;
        lcore->stats.tx_ring_full++;
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
	fprintf(stderr, "Create pktmbuf pool %s num_mbufs %'u cache_size: %'u Priv %'d mbuf_size %'u\n",
		buff, num_mbufs, cache_size, DEFAULT_PRIV_SIZE, mbuf_size);
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

static inline void
begin_time(uint64_t *begin)
{
	if (pinfo->internal_debug_enabled)
		*begin = clock_get_ns();
}

static inline void
end_time(min_avg_max_t *mma, uint64_t begin)
{
	if (pinfo->internal_debug_enabled)
		min_avg_max_update(mma, clock_get_ns() - begin);
}

/// Round up to next higher power of 2 (return x if it's already a power of 2).
static inline uint32_t
pow2roundup(uint32_t x)
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
int port_init(lport_t *lport);
void keyboard_loop(void);
int reference_routine(void *arg);
int mirror_routine(void *arg);

#endif

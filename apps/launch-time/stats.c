/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */
#include "launch-time.h"
#include "rte_version.h"

enum { NAME_WIDTH = 28, COLUMN_WIDTH = 42 };
typedef enum { DST_MAC, SRC_MAC } mac_type_t;

static void
reset_stats(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    memset(&lcore->stats, 0, sizeof(lcore->stats));
    lcore->stats.launch_time.min_ns =
        BIG_NUM;        // Minimum Hardware timestamp time in nanoseconds
    reset_rx_timestamp();
}

static inline void
print_string(const char *name, const char *str)
{
    printf("%-*s: %*s\n", NAME_WIDTH, name, COLUMN_WIDTH, str);
}

static inline void
print_number(const char *name, uint64_t num)
{
    printf("%-*s: %'*" PRIu64 "\n", NAME_WIDTH, name, COLUMN_WIDTH, num);
}

static inline void
print_2_numbers(const char *name, uint64_t num1, uint64_t num2)
{
    printf("%-*s: %'*" PRIu64 "/%'*" PRIu64 "\n", NAME_WIDTH, name, (COLUMN_WIDTH / 2) - 1, num1,
           (COLUMN_WIDTH / 2), num2);
}

static inline void
print_3_numbers(const char *name, uint64_t num1, uint64_t num2, uint64_t num3)
{
    printf("%-*s: %'*" PRIu64 "/%'*" PRIu64 "/%'*" PRIu64 "\n", NAME_WIDTH, name,
           (COLUMN_WIDTH / 3), num1, (COLUMN_WIDTH / 3) - 1, num2, (COLUMN_WIDTH / 3) - 1, num3);
}

static inline void
print_link(void)
{
    char buff[256];

    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    link_status_no_wait(&lcore->lport, buff, sizeof(buff));
    print_string("Link Status", buff);
}

static inline void
print_min_avg_max(min_avg_max_t *rtt, const char *name)
{
    char buff[128];

    snprintf(buff, sizeof(buff), "%-8s Min/Avg/Max ns", name);
    rtt->avg_ns = (rtt->count == 0) ? 0LU : (rtt->sum_ns / rtt->count);
    print_3_numbers(buff, rtt->min_ns, rtt->avg_ns, rtt->max_ns);
}

static inline void
print_rxtx_pps(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    lcore->stats.rx_pps = lcore->lport.stats.ipackets - lcore->lport.stats_prev.ipackets;
    lcore->stats.tx_pps = lcore->lport.stats.opackets - lcore->lport.stats_prev.opackets;
    print_2_numbers("Rx/Tx PPS", lcore->stats.rx_pps, lcore->stats.tx_pps);
}

static inline void
print_errors(void)
{
    const lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_3_numbers("RxMissed/RxError/TxError", lcore->lport.stats.imissed,
                    lcore->lport.stats.ierrors, lcore->lport.stats.oerrors);
}

static inline void
print_mac(mac_type_t type)
{
    char buff[64];
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    if (type == DST_MAC)
        rte_ether_format_addr(buff, sizeof(buff), &lcore->lport.dst_mac);
    else
        rte_ether_format_addr(buff, sizeof(buff), &lcore->lport.src_mac);
    printf("%s-%s", (type == DST_MAC) ? "Dst" : "Src", buff);
}

static inline void
print_total_packets(void)
{
    const lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_2_numbers("Total Rx/Tx Packets", lcore->stats.total_pkts.rx, lcore->stats.total_pkts.tx);
}

static inline void
clear_screen(void)
{
    if (_btst(CLEAR_SCREEN)) {
        _bclr(CLEAR_SCREEN);
        fprintf(stderr, "\033[2J");        // Move cursor to the top left corner
    }
    printf("\033[1;1H");        // Move cursor to the top left corner
}

void
print_stats(void)
{
    static int toggle        = 0;
    static const char *twirl = "|/-\\";
    struct timespec current_time;
    char version[32];
    char time_str[64];

    clear_screen();        // Clear screen and put cursor in column 1, row 1

    if (_btst(RESET_STATS)) {
        _bclr(RESET_STATS);        // Reset stats flag
        reset_stats();
    }

    if (clock_gettime(CLOCK_TAI, &current_time) < 0)
        memset(&current_time, 0, sizeof(current_time));

    // Calculate elapsed time in seconds
    long seconds = current_time.tv_sec - pinfo->start_time.tv_sec;

    // Convert to hours, minutes, and seconds
    int hours             = seconds / 3600;
    int minutes           = (seconds % 3600) / 60;
    int remaining_seconds = seconds % 60;

    // Print the elapsed time in HH:MM:SS format
    snprintf(time_str, sizeof(time_str), "%03d:%02d:%02d", hours, minutes, remaining_seconds);

    printf("%c: %s, Cycles: %'lu ns, ", twirl[toggle++ % 4], "Launch-Time",
           pinfo->launch_interval_ns);
    printf("Burst/Len: %s, Duration:%s, Time:%s\n", pinfo->burst_length_str,
           pinfo->run_duration_str, time_str);

    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    // convert Mbps to Gbps
    double link_speed = (double)(lcore->lport.link.link_speed * 1000000UL);
    // number bits per packet
    double num_bpp = ((double)((pinfo->pkt_length + FCS_SIZE) + 20) * 8.0);
    // number bits per burst
    double num_bpb = (double)pinfo->burst_count * num_bpp;
    // time per burst in nanoseconds
    double wire_time_ns = ((num_bpb / link_speed) * NSEC_PER_SEC);

    printf("   Wire Time:%'.2f ns, bits %'.0f, Delay: %'3d\n", wire_time_ns,
           num_bpb, pinfo->delay_sec);
    printf("   Modes: ");
    printf("%sLogging%s ", _btst(LOG) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("%sMQTT%s ", _btst(MQTT) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("%sPromiscuous%s ", _btst(PROMISCUOUS) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("%sLaunchTime%s ", _btst(LAUNCH_TIME) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("%sHwTimestamp%s ", _btst(HW_TIMESTAMP) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("\n");
    printf("   MAC: ");
    print_mac(DST_MAC);
    printf(" ");
    print_mac(SRC_MAC);
    printf("\n");

    printf("\n");

    rte_memcpy(&lcore->lport.stats_prev, &lcore->lport.stats, sizeof(struct rte_eth_stats));
    rte_eth_stats_get(lcore->lport.pid, &lcore->lport.stats);

    print_link();
    print_min_avg_max(&lcore->stats.launch_time, "Launch");
    print_errors();
    print_total_packets();
    print_rxtx_pps();

    mqtt_stats(&lcore->stats, &lcore->lport.stats);

    if (strlen(RTE_VER_SUFFIX) == 0)
        snprintf(version, sizeof(version), "DPDK %u.%02u.%u", rte_version_year(),
                 rte_version_month(), rte_version_minor());
    else
        snprintf(version, sizeof(version), "DPDK %u.%02u.%u%s%u", rte_version_year(),
                 rte_version_month(), rte_version_minor(), rte_version_suffix(),
                 rte_version_release());
    printf("   [Press q to quit, r to reset stats, c to clear screen] <%s>\n", version);
    fflush(stdout);
}

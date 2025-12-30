/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */
#include "ratt.h"
#include "rte_version.h"

enum { NAME_WIDTH = 33, COLUMN_WIDTH = 42 };
typedef enum { DST_MAC, SRC_MAC } mac_type_t;

void
reset_stats(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    memset(&lcore->stats, 0, sizeof(lcore->stats));
    lcore->stats.rtt.min_ns      = BIG_NUM;        // Minimum RTT time in nanoseconds
    lcore->stats.snapshot.min_ns = BIG_NUM;        // Minimum Snapshot time in nanoseconds
    lcore->stats.spike.min_ns    = BIG_NUM;        // Minimum Spike time in nanoseconds
    lcore->stats.workload.min_ns = BIG_NUM;        // Workload minimum execution time in nanoseconds
    lcore->stats.workload_snapshot.min_ns = BIG_NUM; // Workload snapshot minimum execution time in nanoseconds
}

static inline void
reset_snapshot(void)
{
    lcore_t *lcore          = &pinfo->lcores[pinfo->worker_lcore_id];
    min_avg_max_t *snapshot = &lcore->stats.snapshot;

    memset(snapshot, 0, sizeof(min_avg_max_t));
    snapshot->min_ns = BIG_NUM;

    if (pinfo->rt_workload.enabled) {
        snapshot = &lcore->stats.workload_snapshot;
        memset(snapshot, 0, sizeof(min_avg_max_t));
        snapshot->min_ns = BIG_NUM;
    }
}

static inline void
reset_rxtx_snapshot(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];
    min_avg_max_t *ss;

    ss = &lcore->stats.rx_snapshot;
    memset(ss, 0, sizeof(min_avg_max_t));
    ss->min_ns = BIG_NUM;

    ss = &lcore->stats.tx_snapshot;
    memset(ss, 0, sizeof(min_avg_max_t));
    ss->min_ns = BIG_NUM;
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
print_rtt_cnt(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_number("RTT Count", lcore->stats.rtt.count);
}

static inline void
print_min_avg_max(min_avg_max_t *rtt, const char *name)
{
    char buff[128];

    snprintf(buff, sizeof(buff), "%-17s Min/Avg/Max ns", name);
    rtt->avg_ns    = (rtt->count == 0) ? 0LU : (rtt->sum_ns / rtt->count);
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
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_3_numbers("RxMissed/RxError/TxError", lcore->lport.stats.imissed,
                    lcore->lport.stats.ierrors, lcore->lport.stats.oerrors);
}

static inline void
print_no_mbufs_timestamp(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_2_numbers("No Mbufs/Timestamp", lcore->stats.no_mbufs, lcore->stats.no_timestamp);
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
print_driver(const char * name)
{
    printf("%s", name);
}

static inline void
print_id_tx_full(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_2_numbers("ID Error/Tx Full", lcore->stats.id_error, lcore->stats.tx_ring_full);
}

static inline void
print_rx_timeout(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];
    uint64_t delta_ns;

    delta_ns = (lcore->stats.rx_timeout - lcore->stats.prev_rx_timeout);
    print_2_numbers("Rx Timeout/Per Sec", lcore->stats.rx_timeout, delta_ns);
    lcore->stats.prev_rx_timeout = lcore->stats.rx_timeout;
}

static inline void
print_counts(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_3_numbers("RTT/Snapshot/Spike Count", lcore->stats.rtt.count, lcore->stats.snapshot.count,
                    lcore->stats.spike.count);
}

static inline void
print_total_packets(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_2_numbers("Total Rx/Tx Packets", lcore->stats.total_pkts.rx, lcore->stats.total_pkts.tx);
}

static inline void
print_debug_stats(void)
{
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    print_3_numbers("Rx min/max/avg ns", lcore->stats.rx_snapshot.min_ns, lcore->stats.rx_snapshot.max_ns,
                    lcore->stats.rx_snapshot.avg_ns);
    print_3_numbers("Tx min/max/avg ns", lcore->stats.tx_snapshot.min_ns, lcore->stats.tx_snapshot.max_ns,
                    lcore->stats.tx_snapshot.avg_ns);
}

static inline void
clear_screen(void)
{
    if (pinfo->screen_clear) {
        pinfo->screen_clear = false;
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
    struct rte_eth_dev_info dev_info = {0};

    clear_screen();        // Clear screen and put cursor in column 1, row 1

    if (pinfo->reset_stats) {
        pinfo->reset_stats = false;        // Reset stats flag
        reset_stats();
    }

    clock_gettime(CLOCK_TAI, &current_time);

    // Calculate elapsed time in seconds
    long seconds = current_time.tv_sec - pinfo->start_time.tv_sec;

    // Convert to hours, minutes, and seconds
    int hours             = seconds / 3600;
    int minutes           = (seconds % 3600) / 60;
    int remaining_seconds = seconds % 60;

    // Print the elapsed time in HH:MM:SS format
    snprintf(time_str, sizeof(time_str), "%03d:%02d:%02d", hours, minutes, remaining_seconds);

    printf("%c: %s, Cycles: %'lu ns, ", twirl[toggle++ % 4],
           pinfo->mirror_enabled ? "Mirror" : "Reference", pinfo->cycle_time_ns);
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

    printf("   Wire Time:%'.2f ns, RTT:%'.2f ns, bits %'.0f, Delay: %'3d, Skip:%d\n", wire_time_ns,
           wire_time_ns * 2.0, num_bpb, pinfo->delay_sec, pinfo->pkt_skip_cnt);
    printf("   Modes(!=not): ");
    printf("%sLogging ", pinfo->log_enabled ? "" : "!");
    printf("%sMQTT ", pinfo->mqtt_enabled ? "" : "!");
    printf("%sDeltas ", pinfo->deltas_enabled ? "" : "!");
    printf("%sPromiscuous ", pinfo->promiscuous_mode ? "" : "!");
    printf("%sSerial ", pinfo->mirror_serial_enabled ? "" : "!");
#if HAS_HW_TIMESTAMPING
    printf("%sHwTimestamp ", pinfo->hw_timestamp_enabled ? "" : "!");
#endif
    printf("%sContinueErr ", pinfo->continue_on_error ? "" : "!");
    printf("%sWorkload ", pinfo->rt_workload.enabled ? "" : "!");
    if (pinfo->rt_workload.enabled) {
        printf("    Workload: %s, Function: %s\n", pinfo->rt_workload.file,
            pinfo->rt_workload.func);
        printf("    Workload args: %s\n", pinfo->rt_workload.args);
    } else
	printf("\n");
    printf("   MAC: ");
    print_mac(DST_MAC);
    printf(" ");
    print_mac(SRC_MAC);
    if (rte_eth_dev_info_get(lcore->lport.pid, &dev_info) == 0) {
        printf(" Driver: ");
        print_driver(dev_info.driver_name);
    }
    printf("\n");

    printf("\n");

    rte_memcpy(&lcore->lport.stats_prev, &lcore->lport.stats, sizeof(struct rte_eth_stats));
    rte_eth_stats_get(lcore->lport.pid, &lcore->lport.stats);

    print_link();
    print_min_avg_max(&lcore->stats.rtt, "RTT");
    print_min_avg_max(&lcore->stats.snapshot, "Snapshot");
    print_min_avg_max(&lcore->stats.spike, "Spike");
    print_min_avg_max(&lcore->stats.workload, "Workload");
    print_min_avg_max(&lcore->stats.workload_snapshot, "Workload Snapshot");
    print_counts();
    print_errors();
    print_total_packets();
    print_rxtx_pps();
    print_rx_timeout();
    print_no_mbufs_timestamp();
    print_id_tx_full();

    mqtt_stats(&lcore->stats, &lcore->lport.stats);
    reset_snapshot();

	// Needs to be here after mqtt stats are posted to include Rx/Tx Timing
    if (pinfo->internal_debug_enabled) {
        print_min_avg_max(&lcore->stats.rx_snapshot, "Rx Timing");
        print_min_avg_max(&lcore->stats.tx_snapshot, "Tx Timing");
        reset_rxtx_snapshot();
    }

	if (strlen(RTE_VER_SUFFIX) == 0)
        snprintf(version, sizeof(version), "RTDK %d.%02d.%d", rte_version_year(),
                 rte_version_month(), rte_version_minor());
    else
        snprintf(version, sizeof(version), "RTDK %d.%02d.%d%s%d", rte_version_year(),
                 rte_version_month(), rte_version_minor(), rte_version_suffix(),
                 rte_version_release());
    printf("   [Press q to quit, r to reset stats, c to clear screen] <%s>\n", version);
    fflush(stdout);
}

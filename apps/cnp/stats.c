/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */
#include "cnp-tuning.h"
#include "rte_version.h"

enum { NAME_WIDTH = 28, COLUMN_WIDTH = 42 };
typedef enum { DST_MAC, SRC_MAC } mac_type_t;

void
reset_stats(void)
{
    lport_t *lport = &pinfo->lports[0];        // Assume port 0

    memset(&lport->stats, 0, sizeof(lport->stats));
    memset(&lport->stats_prev, 0, sizeof(lport->stats_prev));
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
    lport_t *lport = &pinfo->lports[0];
    char buff[256];

    link_status_no_wait(lport, buff, sizeof(buff));
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
print_errors(void)
{
    lport_t *lport = &pinfo->lports[0];

    print_3_numbers("RxMissed/RxError/TxError", lport->stats.imissed, lport->stats.ierrors,
                    lport->stats.oerrors);
}

static inline void
print_mac(mac_type_t type)
{
    char buff[64];
    lport_t *lport = &pinfo->lports[0];

    if (type == DST_MAC)
        rte_ether_format_addr(buff, sizeof(buff), &lport->dst_mac);
    else
        rte_ether_format_addr(buff, sizeof(buff), &lport->src_mac);
    printf("%s-%s", (type == DST_MAC) ? "Dst" : "Src", buff);
}

static inline void
print_total_packets(void)
{
    lport_t *lport = &pinfo->lports[0];
    uint64_t rx_delta, rx, tx;

    rx       = lport->stats.ipackets;
    tx       = lport->stats.opackets;
    rx_delta = (tx > rx) ? tx - rx : rx - tx;

    print_3_numbers("Total Rx/Tx/Delta Packets", rx, tx, rx_delta);
}

static inline void
print_rx_packets(void)
{
    lport_t *lport = &pinfo->lports[0];
    uint64_t rx_delta, rx, tx;

    rx       = lport->other_stats.total_pkts.rx;
    tx       = lport->other_stats.total_pkts.tx;
    rx_delta = (tx > rx) ? tx - rx : rx - tx;

    print_3_numbers("Mirror Rx/Tx/Delta Packets", rx, tx, rx_delta);
}

static inline void
print_rxtx_pps(void)
{
    lport_t *lport = &pinfo->lports[0];

    lport->other_stats.rx_pps = lport->stats.ipackets - lport->stats_prev.ipackets;
    lport->other_stats.tx_pps = lport->stats.opackets - lport->stats_prev.opackets;
    print_2_numbers("Rx/Tx PPS", lport->other_stats.rx_pps, lport->other_stats.tx_pps);
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
    lport_t *lport           = &pinfo->lports[0];
    static int toggle        = 0;
    static const char *twirl = "|/-\\";
    char version[32];

    clear_screen();        // Clear screen and put cursor in column 1, row 1

    if (_btst(RESET_STATS)) {
        _bclr(RESET_STATS);        // Reset stats flag
        reset_stats();
    }

    printf("%c: %s, TX Interval: %'lu ns, ", twirl[toggle++ % 4], (pinfo->client_mode) ? "Client Mode" : "Server Mode",
           pinfo->tx_interval_ns);
    printf("Packet Length: %'u\n", pinfo->pkt_length + FCS_SIZE);

    printf("   Modes: ");
    printf("%sPromiscuous%s ", _btst(PROMISCUOUS) ? "\033[32m" : "\033[31m", "\033[0m");
    printf("\n");
    printf("   MAC: ");
    print_mac(DST_MAC);
    printf(" ");
    print_mac(SRC_MAC);
    printf("\n");

	// convert Mbps to Gbps
    double link_speed = (double)(lport->link.link_speed * 1000000UL);
    // number bits per packet
    double num_bpp = ((double)((pinfo->pkt_length + FCS_SIZE) + 20) * 8.0);
    // time per burst in nanoseconds
    double wire_time_ns = ((num_bpp / link_speed) * NSEC_PER_SEC);
	double total_pps = link_speed / num_bpp;
	double pps = (NSEC_PER_SEC / pinfo->tx_interval_ns);

    printf("   Wire Time:%'.2f ns, RTT:%'.2f ns, bits %'.0f,TotalPPS:%'.2f PPS:%'.0f\n", wire_time_ns,
           wire_time_ns * 2.0, num_bpp, total_pps, pps);
    printf("\n");

    rte_eth_stats_get(lport->pid, &lport->stats);

    print_link();
    print_errors();
    print_rx_packets();
    print_total_packets();
    print_rxtx_pps();
    print_number("No Mbufs", lport->other_stats.no_mbufs);

    rte_memcpy(&lport->stats_prev, &lport->stats, sizeof(struct rte_eth_stats));

    if (strlen(RTE_VER_SUFFIX) == 0)
        snprintf(version, sizeof(version), "DPDK %d.%02d.%d", rte_version_year(),
                 rte_version_month(), rte_version_minor());
    else
        snprintf(version, sizeof(version), "DPDK %d.%02d.%d%s%d", rte_version_year(),
                 rte_version_month(), rte_version_minor(), rte_version_suffix(),
                 rte_version_release());
    printf("   [Press q to quit, r to reset stats, c to clear screen] <%s>\n", version);
    fflush(stdout);
}

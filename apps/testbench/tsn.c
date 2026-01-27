// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <stdio.h>
#include <stdlib.h>

#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <sys/mman.h>
#include <termios.h>
#include <poll.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_version.h>
#include "../../external/dpdk/drivers/net/intel/pflow/rte_pflow.h"        // pflow PMD

#include "app_config.h"
#include "config.h"
#include "tsn.h"
#include "dcp_thread.h"
#include "functions.h"
#include "hist.h"
#include "layer2_thread.h"
#include "lldp_thread.h"
#include "log.h"
#include "logviamqtt.h"
#include "m_strings.h"
#include "rta_thread.h"
#include "rtc_thread.h"
#include "stat.h"
#include "thread.h"
#include "tsn_thread.h"
#include "udp_thread.h"
#include "utils.h"

static struct option long_options[] = {
    {"config", required_argument, NULL, 'c'},
    {"mirror", no_argument, NULL, 'm'},
    {"version", no_argument, NULL, 'V'},
    {"pf-port", no_argument, NULL, 'p'},
    {"wait-time", required_argument, NULL, 'w'},
    {"help", no_argument, NULL, 'h'},
    {NULL},
};

bool mirror_mode               = false;
static const char *config_file = NULL;
static int vlan_dynfield_offset;        // Offset into mbuf for the location of VLAN ID
static uint16_t *vid2qid;               // VLAN ID to Qid array
struct timespec start_time;

static void
print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [options]\n", program);
    fprintf(stderr, "  options:\n");
    fprintf(stderr, "    -c | --config <filename> - Path to config file\n");
    fprintf(stderr, "    -m | --mirror            - Start as a mirror instance\n");
    fprintf(stderr, "    -V | --version           - Print version\n");
    fprintf(stderr, "    -h | --help              - Print help text\n");
    fprintf(stderr, "  version: \"%s\"\n", VERSION);
}

static void
print_version(void)
{
    printf("dpdk-tsn version: \"%s\"\n", VERSION);
}

static const struct rte_mbuf_dynfield vlan_dynfield_desc = {
    .name  = "vlan_dynfield",
    .size  = sizeof(uint16_t),
    .align = alignof(uint16_t),
};

static uint16_t *
get_qid(struct rte_mbuf *mbuf)
{
    return RTE_MBUF_DYNFIELD(mbuf, vlan_dynfield_offset, uint16_t *);
}

int
tsn_set_vlan_qid(uint16_t vlan_id, uint16_t queue_id)
{
    if (vid2qid && vlan_id > 0 && vlan_id < RTE_ETHER_MAX_VLAN_ID) {
        vid2qid[vlan_id] = queue_id;
        return 0;
    }
    return -1;
}

uint16_t
tsn_get_vlan_qid(uint16_t vlan_id)
{
    if (vid2qid == NULL)
        return 0;
    return vid2qid[vlan_id];
}

static uint16_t
rx_vlan_callback(uint16_t pid __rte_unused, uint16_t qid __rte_unused, struct rte_mbuf **pkts,
                 uint16_t nb_pkts, uint16_t max_pkts __rte_unused, void *arg __rte_unused)
{
    for (uint16_t i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *m        = pkts[i];
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        uint16_t ether_type       = rte_be_to_cpu_16(eth->ether_type);

        if (ether_type == RTE_ETHER_TYPE_VLAN) {
            struct rte_vlan_hdr *vh =
                rte_pktmbuf_mtod_offset(m, struct rte_vlan_hdr *, sizeof(struct rte_ether_hdr));
            uint16_t vid = rte_be_to_cpu_16(vh->vlan_tci) & 0xFFF;

            *get_qid(m) = vid2qid[vid];
        } else if (ether_type == RTE_ETHER_TYPE_IPV4) {
            struct rte_udp_hdr *udp =
                rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *,
                                        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
            uint16_t src_port = app_config.udp_high_src_port;
            uint16_t vid      = rte_be_to_cpu_16(udp->src_port) == src_port ? UDP_HIGH_FAKE_VLAN_ID
                                                                            : UDP_LOW_FAKE_VLAN_ID;
            *get_qid(m)       = vid2qid[vid];
        } else if (ether_type == RTE_ETHER_TYPE_LLDP) {
            *get_qid(m) = vid2qid[LLDP_FAKE_VLAN_ID];
        } else {
            *get_qid(m) = 0;
            fprintf(stderr, "%s: Unknown packet EtherType: %04x\n", __func__, ether_type);
        }
    }
    return nb_pkts;
}

#define _(prefix, name, id, flg, rx_mbufs, tx_mbufs)                                     \
    do {                                                                                 \
        if (app_config.prefix##_enabled) {                                               \
            lport_conf_t lpc = {0};                                                      \
            lport_set_defaults(id, &lpc);                                                \
            lpc.flags       = flg;                                                       \
            lpc.nb_rx_mbufs = rx_mbufs;                                                  \
            lpc.nb_tx_mbufs = tx_mbufs;                                                  \
            if (lport_setup(#name, id, &lpc))                                            \
                rte_exit(EXIT_FAILURE, "%s lport %s failed\n", #name, lport_format(id)); \
        }                                                                                \
    } while (/*CONSTCOND*/ 0)

static void
tsn_configure_lports(void)
{
	uint16_t phy_port_id = 0, port_id = -1;

	app_config.pkt_handler_physical_lport_id = lport_make(phy_port_id, 0);
	fprintf(stderr, "Physical LPortID for PktHandler: %s\n", lport_format(app_config.pkt_handler_physical_lport_id));

	rte_eth_dev_get_port_by_name("net_pflow", &port_id);
	app_config.pkt_handler_lport_id = lport_make(port_id, 0);
	fprintf(stderr, "         LPortID for PktHandler: %s\n", lport_format(app_config.pkt_handler_lport_id));

    _(pkt_handler, PktHdlrPhy, app_config.pkt_handler_physical_lport_id, 0, (128 * 1024), 0);
    _(pkt_handler, PktHdlr, app_config.pkt_handler_lport_id, 0, 2048, 16384);
    _(tsn_high, tsnHigh, app_config.tsn_high_lport_id, 0, 2048, 8192);
    _(tsn_low, tsnLow, app_config.tsn_low_lport_id, 0, 2048, 8192);
    _(rtc, RTC, app_config.rtc_lport_id, 0, 2048, 2048);
    _(rta, RTA, app_config.rta_lport_id, 0, 2048, 2048);
    _(dcp, DCP, app_config.dcp_lport_id, 0, 2048, 2048);
    _(lldp, LLDP, app_config.lldp_lport_id, 0, 2048, 2048);
    _(udp_high, udpHigh, app_config.udp_high_lport_id, 0, 2048, 2048);
    _(udp_low, udpLow, app_config.udp_low_lport_id, 0, 2048, 2048);
    _(l2, Layer2, app_config.l2_lport_id, 0, 2048, 8192);

    vlan_dynfield_offset = rte_mbuf_dynfield_register(&vlan_dynfield_desc);
    if (vlan_dynfield_offset < 0)
        rte_exit(EXIT_FAILURE, "Cannot register mbuf field\n");

    vid2qid = rte_calloc_socket("vid2qid", RTE_ETHER_MAX_VLAN_ID + 1, sizeof(uint16_t),
                                RTE_CACHE_LINE_SIZE, rte_socket_id());
    if (vid2qid == NULL)
        rte_exit(EXIT_FAILURE, "unable to allocate vid2qid array\n");

    pflow_conf_t cfg = {0};
    cfg.phy_pid      = lport2pid(app_config.pkt_handler_physical_lport_id);
    cfg.phy_qid      = lport2qid(app_config.pkt_handler_physical_lport_id);
    cfg.get_fn       = get_qid;
    cfg.cb_fn        = rx_vlan_callback;
    cfg.cb_arg       = NULL;

    if (pflow_callback_create(lport2pid(app_config.pkt_handler_lport_id), &cfg) < 0)
        rte_exit(EXIT_FAILURE, "Setting up pflow PMD failed\n");
}

#undef _

static void
tsn_startup(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        rte_exit(EXIT_FAILURE, "mlockall() failed: %s\n", strerror(errno));

    configure_cpu_latency();

    function_init_all();

    if (function_link_pn_threads())
        rte_exit(EXIT_FAILURE, "Cannot link PN threads\n");
}

static void
tsn_launch(void)
{
    function_launch_all();
}

static struct termios oldterm;        // Old terminal setup information
volatile bool force_quit  = false;
volatile bool scrn_clear  = true;
volatile bool reset_stats = true;
volatile bool tty_inited  = false;

static void
tsn_shutdown(void)
{
    stats_reset_all_stats();
    rte_eal_mp_wait_lcore();

    force_quit = true;

    if (!mirror_mode)
        histogram_write();

    function_free_all();

    restore_cpu_latency();

    munlockall();
}

static volatile sig_atomic_t sig_caught;

static void
signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        sig_caught = 1;
        lport_link_running_set(0);
    }
}

static inline void
cpos(int row, int col)
{
    printf("\033[%d;%dH", row, col);        // Move cursor to row/col
}

static inline void
home(void)
{
    cpos(1, 1);
}

static inline void
cls(void)
{
    home();
    printf("\033[2J");        // clear screen
}

static inline void
screen_clear(void)
{
    if (scrn_clear) {
        scrn_clear = false;
        cls();
    } else
        home();
}

static int
stdin_setup(void)
{
    struct termios term;

    tty_inited = false;
    if (tcgetattr(0, &oldterm)) {
        fprintf(stderr, "%s: failed to get tty\n", __func__);
        return -1;
    }

    memcpy(&term, &oldterm, sizeof(term));

    term.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);

    if (tcsetattr(0, TCSANOW, &term)) {
        fprintf(stderr, "%s: failed to set tty\n", __func__);
        return -1;
    }
    tty_inited = true;

    return 0;
}

static inline void
stdin_restore(void)
{
    if (tty_inited && tcsetattr(0, TCSANOW, &oldterm))
        fprintf(stderr, "%s: failed to set tty\n", __func__);
}

static inline void
sleep_usec(uint32_t usec)
{
    if (usec > 0) {
        struct timespec wakeup_time;
        int ret;

        uint64_t nsec = (uint64_t)(usec * 1000UL);        // convert to nanoseconds

        if (clock_gettime(CLOCK_TAI, &wakeup_time) < 0)
            return;

        wakeup_time.tv_nsec += nsec;
        while (wakeup_time.tv_nsec >= NSEC_PER_SEC) {
            wakeup_time.tv_sec++;
            wakeup_time.tv_nsec -= NSEC_PER_SEC;
        }

        do {
            ret = clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
        } while (ret == EINTR);
    }
}

#define TITLE_WIDTH 18
#define DATA_WIDTH  36

static void
print2(int row, int col, uint64_t d1, uint64_t d2)
{
    char out[128];

    cpos(row++, col);
    snprintf(out, sizeof(out), "%'*" PRIu64 "/%'*" PRIu64 "|", (DATA_WIDTH / 2) - 1, d1,
             (DATA_WIDTH / 2) - 1, d2);
    printf("%*s", DATA_WIDTH, out);
}

static void
print3(int row, int col, uint64_t ipkts, uint64_t opkts, uint64_t errs)
{
    char out[128];

    cpos(row++, col);
    snprintf(out, sizeof(out), "%'*" PRIu64 "/%'*" PRIu64 "/%'*" PRIu64 "|", DATA_WIDTH / 3, ipkts,
             (DATA_WIDTH / 3) - 1, opkts, (DATA_WIDTH / 3) - 2, errs);
    printf("%*s", DATA_WIDTH, out);
}

static void
print_stats(void)
{
    struct rte_eth_stats eth_stats;
    static struct rte_eth_stats prev_stats[2] = {0};
    static int count                          = 0;
    struct timespec current_time;
    int row, col, saved_row, last_row;
    char time_str[64];
    // clang-format off
	const char *tsn_titles[] = {
		"RTTMin (us)",
		"RTTAvg (us)",
		"RTTMax (us)",
		"OWL Avg (us)",
		"OWL outliers",
        "RTT outliers"
	};
	const char *stats_titles[] = {
		"LinkState",
		"Rx/Tx Packets",
		"Rx/Tx PPS",
		"Rx Err/Mis/TxErr",
		"Q0-in/out/err",
		"TSNH-in/out/err",
		"TSNL-in/out/err",
		"RTC -in/out/err",
		"RTA -in/out/err",
		"DCP -in/out/err",
		"LLDP-in/out/err",
		"UDPH-in/out/err",
		"UDPL-in/out/err",
		"L2  -in/out/err"
	};
    // clang-format on
    char buff[32], out[128];

    screen_clear();
    if (reset_stats) {
        reset_stats = false;
        stats_reset_all_stats();
    }

    if (clock_gettime(CLOCK_TAI, &current_time) < 0)
        memset(&current_time, 0, sizeof(current_time));

    // Calculate elapsed time in seconds
    long seconds = current_time.tv_sec - start_time.tv_sec;

    // Convert to hours, minutes, and seconds
    int hours             = seconds / 3600;
    int minutes           = (seconds % 3600) / 60;
    int remaining_seconds = seconds % 60;

    // Print the elapsed time in HH:MM:SS format
    snprintf(time_str, sizeof(time_str), "%03d:%02d:%02d", hours, minutes, remaining_seconds);

    printf("%c  %s: CycleTime:%'" PRIu64 "ns Rx:%'" PRIu64 "ns Tx:%'" PRIu64 "ns RTTAvg:~%'" PRIu64
           "ns Elapsed Time:%s\n",
           "|/-\\"[count++ % 4], is_mirror_mode() ? "Mirror" : "Reference",
           app_config.application_base_cycle_time_ns, app_config.application_rx_base_offset_ns,
           app_config.application_tx_base_offset_ns,
           app_config.application_base_cycle_time_ns + ((app_config.application_base_cycle_time_ns -
                                                         app_config.application_tx_base_offset_ns) +
                                                        app_config.application_rx_base_offset_ns),
           time_str);

    printf("%-10s", "Frame ID");
    for (uint32_t i = 0; i < RTE_DIM(tsn_titles); i++)
        printf("%14s", tsn_titles[i]);
    printf("%12s\n", "Pkts/Cycle");

    row = 1;
    col = 0;
    for (int i = 0; i < NUM_FRAME_TYPES; i++) {
        struct statistics *stats;
        uint16_t ppc = 0;

        switch (i) {
        case TSN_HIGH_FRAME_TYPE:
            if (!app_config.tsn_high_enabled)
                continue;
            ppc = app_config.tsn_high_num_frames_per_cycle;
            break;
        case TSN_LOW_FRAME_TYPE:
            if (!app_config.tsn_low_enabled)
                continue;
            ppc = app_config.tsn_low_num_frames_per_cycle;
            break;
        case RTC_FRAME_TYPE:
            if (!app_config.rtc_enabled)
                continue;
            ppc = app_config.rtc_num_frames_per_cycle;
            break;
        case RTA_FRAME_TYPE:
            if (!app_config.rta_enabled)
                continue;
            ppc = app_config.rta_num_frames_per_cycle;
            break;
        case DCP_FRAME_TYPE:
            if (!app_config.dcp_enabled)
                continue;
            ppc = app_config.dcp_num_frames_per_cycle;
            break;
        case LLDP_FRAME_TYPE:
            if (!app_config.lldp_enabled)
                continue;
            ppc = app_config.lldp_num_frames_per_cycle;
            break;
        case UDP_HIGH_FRAME_TYPE:
            if (!app_config.udp_high_enabled)
                continue;
            ppc = app_config.udp_high_num_frames_per_cycle;
            break;
        case UDP_LOW_FRAME_TYPE:
            if (!app_config.udp_low_enabled)
                continue;
            ppc = app_config.udp_low_num_frames_per_cycle;
            break;
        case L2_FRAME_TYPE:
            if (!app_config.l2_enabled)
                continue;
            ppc = app_config.l2_num_frames_per_cycle;
            break;
        }
        stats = stat_get_global_statistics(i);
        printf("%-10s", stat_frame_type_to_string(i));
        printf("%'14" PRIu64, stats->round_trip_min);
        printf("%'14.2f", stats->round_trip_avg);
        printf("%'14" PRIu64, stats->round_trip_max);
        printf("%'14.2f", stats->oneway_avg);
        printf("%'14" PRIu64, stats->oneway_outliers);
        printf("%'14" PRIu64, stats->round_trip_outliers);
        printf("%'12" PRIu16, ppc);
        printf("\n");
        row++;
    }
    printf("---------------------------------------------------------------------------------------"
           "----\n");
    row += 3;
    saved_row = row;
    for (uint32_t i = 0; i < RTE_DIM(stats_titles); i++) {
        cpos(row++, 1);
        printf("%-*s|", TITLE_WIDTH, stats_titles[i]);
    }
    printf("\n     [Press q to quit, c to clear screen, r to reset stats]\n");
	last_row = row;

    row = saved_row;
    col = TITLE_WIDTH + 2;
    for (int pid = 0; pid < rte_eth_dev_count_avail(); pid++) {
        struct rte_eth_stats *p = &prev_stats[pid];

        rte_eth_stats_get(pid, &eth_stats);

        col += (DATA_WIDTH * pid);

        cpos(row++, col);
        snprintf(out, sizeof(out) - 1, "%d-%s|", pid,
                 lport_link_string(lport_make(pid, 0), buff, sizeof(buff)));
        printf("%*s", DATA_WIDTH, out);

        print2(row++, col, eth_stats.ipackets, eth_stats.opackets);
        print2(row++, col, eth_stats.ipackets - p->ipackets, eth_stats.opackets - p->opackets);
        print3(row++, col, eth_stats.ierrors, eth_stats.imissed, eth_stats.oerrors);

        row = saved_row;
        memcpy(p, &eth_stats, sizeof(struct rte_eth_stats));
    }

    printf("\n");
    cpos(last_row, 0);

    fflush(stdout);
}

static int
poll_keyboard(uint8_t *c)
{
    struct pollfd fds;

    fds.fd      = 0;
    fds.events  = POLLIN;
    fds.revents = 0;

    if (poll(&fds, 1, 0)) {
        if ((fds.revents & (POLLERR | POLLNVAL)) == 0) {
            if ((fds.revents & POLLHUP))
                return -1;
            else if ((fds.revents & POLLIN)) {
                int n = read(fds.fd, c, 1);
                if (n > 0)
                    return 1;
            }
        }
    }
    return 0;
}

static void
keyboard(void)
{
    uint32_t count = 0;
    uint8_t c;

    pthread_detach(pthread_self());

    stdin_setup();

    sleep_usec(2000000);

    if (clock_gettime(CLOCK_TAI, &start_time) < 0)
        memset(&start_time, 0, sizeof(start_time));

    // scroll the screen to save messages, after clear screen
    for (int i = 0; i < 64; i++)
        printf("\n");

    while (force_quit == false) {
        if ((count++ % 4) == 0)
            print_stats();

        if (poll_keyboard(&c)) {
            switch (c) {
            case 'q':
                force_quit = true;
                goto leave;
            case 'r':
                reset_stats = true;
                /* FALL-THRU */
            case 'c':
                scrn_clear = true;
                break;
            default:
                fprintf(stderr, "Unknown command: %c\n", c);
                break;
            }
        }
        sleep_usec(250000);        // 250ms
    }
leave:
    print_stats();
    cpos(99, 1);
    stdin_restore();
    function_stop_all();
    thread_timer_stop_all();
    force_quit = true;
}

int
main(int argc, char *argv[])
{
    int c, ret;

    if (signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGTERM, signal_handler) == SIG_ERR ||
        signal(SIGUSR1, signal_handler) == SIG_ERR)
        rte_exit(EXIT_FAILURE, "Error: Failed to register signal handler\n");

    setlocale(LC_ALL, "");

    rte_set_application_usage_hook(print_usage);

    /* Initialize DPDK */
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "Error initializing %s: %s\n", rte_version(), rte_strerror(-ret));
        exit(EXIT_FAILURE);
    }
    // Skip DPDK arguments from command line and process application arguments
    argc -= ret;
    argv += ret;

    fprintf(stderr, "Initializing %s with %u core(s), Core ID: %u\n", rte_version(),
            rte_lcore_count(), rte_lcore_id());
    fprintf(stderr, "  Number of ports available %u\n", rte_eth_dev_count_avail());
    fprintf(stderr, "  Total Number of ports     %u\n", rte_eth_dev_count_total());

    if (rte_lcore_count() < (MAX_PN_TYPES + 1))
        rte_exit(EXIT_FAILURE, "Not enough cores (%u) to run all required threads (Need %d+1)!\n",
                 rte_lcore_count(), MAX_PN_TYPES);

    mirror_mode = false;
    config_file = NULL;
    while ((c = getopt_long(argc, argv, "Vc:mh", long_options, NULL)) != -1) {
        switch (c) {
        case 'V':
            print_version();
            exit(EXIT_SUCCESS);
            break;
        case 'c':
            config_file = optarg;
            break;
        case 'm':
            mirror_mode = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
            break;
        }
    }
    config_set_file(config_file);

    printf(">> %s mode selected\n", mirror_mode ? "Mirror" : "Reference");

	printf(">> TSN Testbench Startup Version: %s\n", VERSION);
    tsn_startup();

	printf(">> Configuring lports...\n");
    tsn_configure_lports();

    // wait_sync_time(app_config.application_wait_time_before_start);

	printf(">> Launching TSN Testbench...\n");
    tsn_launch();

	printf(">> TSN Testbench Running keyboard...\n");
    keyboard();

    tsn_shutdown();

    return EXIT_SUCCESS;
}

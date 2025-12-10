/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "launch-time.h"
#include "log.h"
#include "mqtt.h"

void
print_app_usage(const char *prgname)
{
    // clang-format off
    printf("\n");
    printf("%s: Application Options:\n", prgname);
    printf("  Required:\n");
    printf("    -r | --reference         Enable Reference mode (Default Enabled)\n");
    printf("    -m | --mirror            Enable Mirror mode (Default Disabled)\n");
    printf("    -c | --launch-interval N Number of nanoseconds per launch time interval (e.g., 31250ns = 31.250us)\n");
    printf("    -b | --burst-length N    Burst-Count/Pkt-Length (e.g., 1/64 or 4/128, max burst: %'d)\n", MAX_BURST_COUNT);
    printf("\n");
    printf("  Optional:\n");
    printf("    -d | --dest-mac MAC      Destination MAC address (default: FF:FF:FF:FF:FF:FF)\n");
    printf("    -l | --log-file FILE     Log packet timestamps to FILE\n");
    printf("    -M | --mqtt              Enable MQTT logging (Default Disabled)\n");
    printf("    -s | --link-speed N      Desired NIC Link Speed in Mbps (Default Auto-Neg)\n");
    printf("    -R | --run-duration N    Amount of time to run 'Hours:Minutes:Seconds' (default forever) (Reference)\n");
    printf("    -P | --promiscuous       Enable promiscuous mode (Default Disabled)\n");
    printf("    -i | --internal-debug    Internal debugging statistics\n");
    printf("    -L | --launch-time       Enable launch time support\n");
    printf("    -H | --hw-timestamp      Enable hardware timestamping\n");
    printf("    -h | --help              Print this help text\n");
    printf("    -D | --delay-time N      Number of seconds to delay (Default: %'d) (Reference)\n", DEFAULT_DELAY_SEC);
    printf("    -T | --tx-burst-offset N TX burst offset in ns before cycle end (Default: auto-calculated)\n");
    printf("\n");
    // clang-format on
}

static int
count_chr(const char *str, char c)
{
    int count = 0;
    while (*str) {
        if (*str == c)
            count++;
        str++;
    }
    return count;
}

static void
process_duration(char *str)
{
    uint32_t hours = 0, minutes = 0, seconds = 0;
    char dur_str[64];

    switch (count_chr(str, ':')) {
    case 0:        // no colons if must be seconds
        sscanf(str, "%d", &seconds);
        break;
    case 1:                       // one colon if must be minutes and seconds
        if (str[0] == ':')        // no minutes, just seconds
            sscanf(str, ":%d", &seconds);
        else
            sscanf(str, "%d:%d", &minutes, &seconds);
        break;
    case 2:        // two colons if must be hours, minutes, and seconds
        if (str[0] == ':' && str[1] == ':')        // no hours or minutes, just seconds
            sscanf(str, "::%d", &seconds);
        else if (str[0] == ':')        // no hours, just minutes and seconds
            sscanf(str, ":%d:%d", &minutes, &seconds);
        else
            sscanf(str, "%d:%d:%d", &hours, &minutes, &seconds);
        break;
    default:
        rte_exit(EXIT_FAILURE,
                 "Error: Invalid run duration format, expected format: Hours:Minutes:Seconds\n");
    }
    pinfo->run_duration_sec += hours * 60 * 60 + minutes * 60 + seconds;
    snprintf(dur_str, sizeof(dur_str), "%03d:%02d:%02d", hours, minutes, seconds);
    free(pinfo->run_duration_str);
    pinfo->run_duration_str = strdup(dur_str);
}

/* Parse the commandline arguments. */
int
parse_args(int argc, char **argv)
{
    int opt;
    char **argvopt;
    int option_index;
    char *prgname = argv[0];
    // clang-format off
    static struct option lgopts[] = {
		// Required options
		{"launch-interval", required_argument, 0, 'c'},
		{"burst-length", required_argument, 0, 'b'},
		// Optional options
		{"dest-mac", required_argument, 0, 'd'},
		{"log-file", required_argument, 0, 'l'},
		{"mqtt", no_argument, 0, 'M'},
		{"link-speed", required_argument, 0, 's'},
		{"run-duration", required_argument, 0, 'R'},
		{"promiscuous", no_argument, 0, 'P'},
		{"launch-time", no_argument, 0, 'L'},
		{"hw-timestamp", no_argument, 0, 'H'},
		{"help", no_argument, 0, 'h'},
		{"delay-time", required_argument, 0, 'D'},
		{"tx-burst-offset", required_argument, 0, 'T'},
		{NULL, 0, 0, 0}
    };
    // clang-format on
    const char *short_options = "c:b:d:l:Ms:R:D:T:PLHh";
    argvopt                   = argv;

    pinfo->delay_sec           = DEFAULT_DELAY_SEC;
    pinfo->link_speed          = RTE_ETH_SPEED_NUM_UNKNOWN;
    pinfo->run_duration_str    = strdup("000:00:00");
    pinfo->dest_mac_str        = strdup("FF:FF:FF:FF:FF:FF");
    pinfo->rx_timestamp_offset = -1;
    pinfo->tx_timestamp_offset = -1;

    // Parse the command line options.
    while ((opt = getopt_long(argc, argvopt, short_options, lgopts, &option_index)) != EOF) {

        switch (opt) {
        case 'c':        // launch-interval
            pinfo->launch_interval_ns = strtoul(optarg, NULL, 0);
            printf(">> Launch Interval Set To: %" PRIu64 " ns\n", pinfo->launch_interval_ns);
            break;
        case 'b':        // burst-length
            switch (count_chr(optarg, '/')) {
            case 1:
                sscanf(optarg, "%hd/%hd", &pinfo->burst_count, &pinfo->pkt_length);
                break;
            default:
                rte_exit(EXIT_FAILURE, "Error: Invalid format, expected format: Burst/Length\n");
            }
            if (pinfo->burst_count > MAX_BURST_COUNT)
                rte_exit(EXIT_FAILURE, "Error: burst size must be less than or equal to %d\n",
                         MAX_BURST_COUNT);

            if (pinfo->pkt_length > MAX_PKT_LENGTH)
                pinfo->pkt_length = MAX_PKT_LENGTH;
            else if (pinfo->pkt_length < MIN_PKT_LENGTH)
                pinfo->pkt_length = MIN_PKT_LENGTH;

            pinfo->pkt_length -= FCS_SIZE;        // remove the FCS bytes
            pinfo->burst_length_str = strdup(optarg);
            printf(">> Burst Length Set To: %s\n", pinfo->burst_length_str);
            break;
        case 'd':        // dest-mac
            free(pinfo->dest_mac_str);
            pinfo->dest_mac_str = strdup(optarg);
            printf(">> Destination MAC Set To: %s\n", pinfo->dest_mac_str);
            break;
        case 'D':        // Delay start TX in seconds
            pinfo->delay_sec = atoi(optarg);
            printf(">> Delay Start TX Set To: %d seconds\n", pinfo->delay_sec);
            break;
        case 'l':        // log-file
            if (!_btst(LOG) && log_init(optarg))
                rte_exit(EXIT_FAILURE, "Error: Failed to open log file\n");
            _bset(LOG);
            printf(">> Log File Enabled\n");
            break;
        case 'M':        // MQTT enabled
            if (!_btst(MQTT) && mqtt_init())
                rte_exit(EXIT_FAILURE, "Error: Failed to initialize MQTT\n");
            _bset(MQTT);
            printf(">> MQTT Logging Enabled\n");
            break;
        case 's':        // link speed Mbps
            pinfo->link_speed = strtoul(optarg, NULL, 0);
            printf(">> Link Speed Set To: %u Mbps\n", pinfo->link_speed);
            break;
        case 'R':        // Run duration in seconds
            process_duration(optarg);
            printf(">> Run Duration Set To: %s\n", optarg);
            break;
        case 'P':        // promiscuous mode
            _bset(PROMISCUOUS);
            printf(">> Promiscuous Mode Enabled\n");
            break;
        case 'L':        // launch time mode
            _bset(LAUNCH_TIME);
            printf(">> Launch Time Enabled\n");
            break;
        case 'H':        // hardware timestamp mode
            _bset(HW_TIMESTAMP);
            printf(">> HW Timestamping Enabled\n");
            break;
        case 'T':        // TX burst offset in nanoseconds
            pinfo->tx_burst_offset_ns = strtoul(optarg, NULL, 0);
            printf(">> TX Burst Offset Set To: %u ns\n", pinfo->tx_burst_offset_ns);
            break;
        case 'h':
            print_app_usage(prgname);
            exit(0);
            break;

        default:
            print_app_usage(prgname);
            rte_exit(EXIT_FAILURE, "Invalid argument\n");
            break;
        }
    }

    if (pinfo->launch_interval_ns == 0 || pinfo->burst_count == 0 || pinfo->pkt_length == 0)
        rte_exit(EXIT_FAILURE,
                 "Error: Invalid arguments, must contain cycle time and Burst/Length\n");

    // Set reasonable default for tx_burst_offset_ns if not specified
    if (pinfo->tx_burst_offset_ns == 0) {
        // Default to 2% of cycle time or 60us, whichever is smaller
        uint32_t calculated_offset = pinfo->launch_interval_ns / 50;        // 2% of cycle time
        pinfo->tx_burst_offset_ns  = (calculated_offset < 60000) ? calculated_offset : 60000;
        printf(">> TX Burst Offset Auto-Set To: %u ns (based on cycle time)\n",
               pinfo->tx_burst_offset_ns);
    }

    argv[optind - 1] = prgname;

    optind = 1; /* Reset getopt lib. */

    return 0;
}

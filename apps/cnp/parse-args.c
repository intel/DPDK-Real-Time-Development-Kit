/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"

void
print_app_usage(const char *prgname)
{
    // clang-format off
    printf("\n");
    printf("%s: Application Options:\n", prgname);
    printf("  Required:\n");
    printf("    -t | --tx-interval N     Number of nanoseconds per TX interval (e.g., 31250ns = 31.250us)\n");
    printf("    -l | --pkt-length N      Length of packet (Min: %d, Max: %d) Default: 64\n", MIN_PKT_LENGTH, MAX_PKT_LENGTH);
    printf("\n");
    printf("  Optional:\n");
    printf("    -c | --client IP:port    Enable Client mode <IP:port> for client mode only\n");
    printf("    -d | --dst-mac MAC       Destination MAC address (default: FF:FF:FF:FF:FF:FF)\n");
    printf("    -D | --debug             Enable debug mode (Default Disabled)\n");
    printf("    -P | --promiscuous       Enable promiscuous mode (Default Disabled)\n");
    printf("    -h | --help              Print this help text\n");
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
		{"tx-interval", required_argument, 0, 't'},
		{"pkt-length", required_argument, 0, 'l'},
		// Optional
		{"client", required_argument, 0, 'c'}, // Used for client mode only
		{"dst-mac", required_argument, 0, 'd'},
		{"promiscuous", no_argument, 0, 'P'},
		{"debug", no_argument, 0, 'D'},
		{"help", no_argument, 0, 'h'},
		{NULL, 0, 0, 0}
    };
    // clang-format on
    const char *short_options = "c:t:l:d:PhD";
    argvopt                   = argv;

    pinfo->dst_mac_str         = strdup("FF:FF:FF:FF:FF:FF");

    // Parse the command line options.
    while ((opt = getopt_long(argc, argvopt, short_options, lgopts, &option_index)) != EOF) {

        switch (opt) {
        case 'c':        // client
            if (count_chr(optarg, ':') != 1)
                rte_exit(EXIT_FAILURE, "Invalid client IP:port format\n");
            free(pinfo->client_addr_str);
            pinfo->client_addr_str = strdup(optarg);
            printf(">> Client Mode Enabled, Remote Address Set To: %s\n", pinfo->client_addr_str);
			pinfo->client_mode = true;
            break;
        case 't':        // tx-interval
            pinfo->tx_interval_ns = strtoul(optarg, NULL, 0);
            printf(">> TX Interval Set To: %" PRIu64 " ns\n", pinfo->tx_interval_ns);
            break;
        case 'l':        // pkt-length
            pinfo->pkt_length = atoi(optarg);

            if (pinfo->pkt_length > MAX_PKT_LENGTH)
                pinfo->pkt_length = MAX_PKT_LENGTH;
            else if (pinfo->pkt_length < MIN_PKT_LENGTH)
                pinfo->pkt_length = MIN_PKT_LENGTH;

            pinfo->pkt_length -= FCS_SIZE;        // remove the FCS bytes
            printf(">> Packet Length Set To: %d minus %d (FCS)\n", pinfo->pkt_length, FCS_SIZE);
            break;
        case 'd':        // dst-mac
            free(pinfo->dst_mac_str);
            pinfo->dst_mac_str = strdup(optarg);
            printf(">> Destination MAC Set To: %s\n", pinfo->dst_mac_str);
            break;
		case 'D': // Debug mode
			_bset(DEBUG_MODE);
			printf(">> Debug Mode Enabled\n");
			break;
        case 'P':        // promiscuous mode
            _bset(PROMISCUOUS);
            printf(">> Promiscuous Mode Enabled\n");
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

    if (pinfo->tx_interval_ns == 0 || pinfo->pkt_length == 0)
        rte_exit(EXIT_FAILURE,
                 "Error: Invalid arguments, must contain TX interval time and Packet Length\n");

    argv[optind - 1] = prgname;

    optind = 1; /* Reset getopt lib. */

    return 0;
}

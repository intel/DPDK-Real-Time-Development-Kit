/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "ratt.h"
#include "log.h"
#include "mqtt.h"
#include <dlfcn.h>

void string_to_argc_argv(const char *input, int *argc, char ***argv);

enum {
    OPT_SKIP_NUM = 0,
    OPT_DELAY_NUM,
    OPT_CONTINUE_ON_ERROR_NUM,
#if HAS_HW_TIMESTAMPING
    OPT_HW_TIMESTAMP_NUM,
#endif
};

static void
print_usage(const char *prgname)
{
    // clang-format off
    printf("%s [EAL options] -- <app-options>\n"
           "required arguments:\n"
           "  -r | --reference      - Enable Reference (Default Enabled)\n"
           "  -m | --mirror         - Enable Mirror (Default Disabled)\n"
           "  -c | --cycle-time N   - Number of nanoseconds per cycle i.e., 31250ns = 31.250us\n"
           "  -b | --burst-length N - Burst-Count/Pkt-Length e.g. 1/64 or 4/128 (max burst: %'d)\n"
		   "optional arguments:\n"
           "  -d | --dest-mac MAC   - Destination MAC address (default: FF:FF:FF:FF:FF:FF)\n"
           "  -l | --log-file FILE  - Log packet timestamps to FILE\n"
           "  -M | --mqtt           - Enable MQTT logging (Default Disabled)\n"
           "  -D | --deltas         - Enable logging of deltas (Default Disabled)\n"
           "  -s | --link-speed     - Desired NIC Link Speed in Mbps (Default Auto-Neg)\n"
           "  -R | --run-duration   - Amount of time to run 'Hours:Minutes:Seconds' (default forever) (Reference)\n"
           "  -P | --promiscuous    - Enable promiscuous mode (Default Disabled)\n"
		   "  -S | --mirror-serial  - Enable mirror to serialize packets (Default Disabled)\n"
		   "  -i | --internal-debug - Internal debugging statistics\n"
           "  -h | --help           - Print this help text\n"
           "  -w | --workload       - Real-time workload to execute, e.g. filename,function,parameters\n"
           "  --skip-count          - Number packets to skip (Default %d) (Reference)\n"
           "  --delay-time          - Number of seconds to delay (Default: %'d) (Reference)\n"
           "  --continue-on-err     - Continue running on timing validation errors (Default Disabled) (Reference)\n"
#if HAS_HW_TIMESTAMPING
           "  --hw-timestamp        - Enable hardware timestamping\n"
#endif
		   "[EAL options] are required\n",
           prgname, MAX_BURST_COUNT, DEFAULT_SKIP_COUNT, DEFAULT_DELAY_SEC);
    // clang-format on
    exit(0);
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

/**
 * Remove leading and trailing white space from a string.
 *
 * @param str
 *   String to be trimmed, must be null terminated
 * @return
 *   pointer to the trimmed string or NULL if \p str is Null or
 *   if string is empty then return pointer to \p str
 */
static __inline__ char *
strtrim(char *str)
{
    if (!str || !*str)
        return str;

    /* trim white space characters at the front */
    while (isspace(*str))
        str++;

    /* Make sure the string is not empty */
    if (*str) {
        char *p = &str[strlen(str) - 1];

        /* trim trailing white space characters */
        while ((p >= str) && isspace(*p))
            p--;

        p[1] = '\0';
    }
    return str;
}

/**
 * Parse a string into a \p argv list using a set of delimiters, but does
 * handle quoted strings within the string being parsed
 *
 * @param str
 *   String to be tokenized and will be modified, null terminated
 * @param delim
 *   A null terminated list of delimiters
 * @param argv
 *   A pointer to an array to place the token pointers
 * @param maxtokens
 *   Max number of tokens to be placed in \p entries
 * @return
 *   The number of tokens in the \p entries array.
 */
static __inline__ int
strqtok(char *str, const char *delim, char *argv[], int maxtokens)
{
    char *p, *start_of_word, *s;
    int argc                                                             = 0;
    enum { INIT, WORD, STRING_QUOTE, STRING_TICK, STRING_BRACKET } state = WORD;

    if (!str || !delim || !argv || maxtokens == 0)
        return -1;

    /* Remove white space from start and end of string */
    s = strtrim(str);

    start_of_word = s;
    for (p = s; (argc < maxtokens) && (*p != '\0'); p++) {
        int c = (unsigned char)*p;

        if (c == '\\') {
            start_of_word = ++p;
            continue;
        }

        switch (state) {
        case INIT:
            if (c == '"') {
                state         = STRING_QUOTE;
                start_of_word = p + 1;
            } else if (c == '\'') {
                state         = STRING_TICK;
                start_of_word = p + 1;
            } else if (c == '{') {
                state         = STRING_BRACKET;
                start_of_word = p + 1;
            } else if (!strchr(delim, c)) {
                state         = WORD;
                start_of_word = p;
            }
            break;

        case STRING_QUOTE:
            if (c == '"') {
                *p           = 0;
                argv[argc++] = start_of_word;
                state        = INIT;
            }
            break;

        case STRING_TICK:
            if (c == '\'') {
                *p           = 0;
                argv[argc++] = start_of_word;
                state        = INIT;
            }
            break;

        case STRING_BRACKET:
            if (c == '}') {
                *p           = 0;
                argv[argc++] = start_of_word;
                state        = INIT;
            }
            break;

        case WORD:
            if (strchr(delim, c)) {
                *p            = 0;
                argv[argc++]  = start_of_word;
                state         = INIT;
                start_of_word = p + 1;
            }
            break;

        default:
            break;
        }
    }

    if ((state != INIT) && (argc < maxtokens))
        argv[argc++] = start_of_word;

    if ((argc == 0) && (p != str))
        argv[argc++] = str;

    argv[argc] = NULL;

    return argc;
}

static void process_workload(char *arg)
{
    char *token;
    char extraParams[WORKLOAD_MAX_ARGS] = "";
    int firstExtra = 1;

    token = strtok(arg, ",");
    while (token != NULL) {
        if (strncmp(token, "file=", 5) == 0) {
            pinfo->rt_workload.file = token + 5;
        }
        else if (strncmp(token, "func=", 5) == 0) {
            pinfo->rt_workload.func = token + 5;
        }
        else {
            if (!firstExtra) {
                strncat(extraParams, ",", WORKLOAD_MAX_ARGS - strlen(extraParams) - 1);
            }
            strncat(extraParams, token, WORKLOAD_MAX_ARGS - strlen(extraParams) - 1);
            firstExtra = 0;
        }
        token = strtok(NULL, ",");
    }

    strcpy(pinfo->rt_workload.args, extraParams);

    // Tokenize extraParams into argc argv
    pinfo->rt_workload.workload_argc = strqtok(extraParams, " \r\n", pinfo->rt_workload.workload_argv, WORKLOAD_MAX_ARGS);

    pinfo->rt_workload.workload_handler = dlopen(pinfo->rt_workload.file, RTLD_NOW | RTLD_GLOBAL);
    if (!pinfo->rt_workload.workload_handler) {
        stop_running();
        rte_exit(EXIT_FAILURE, "Error: Unable to open workload: %s\n", pinfo->rt_workload.file);
    }

    pinfo->rt_workload.workload_function = dlsym(pinfo->rt_workload.workload_handler, pinfo->rt_workload.func);
    if (!pinfo->rt_workload.workload_function) {
        stop_running();
        rte_exit(EXIT_FAILURE, "Error: Unable to find function: %s , in %s.\n", pinfo->rt_workload.func, pinfo->rt_workload.file);
    }

    pinfo->rt_workload.enabled = true;

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
		{"reference", 		no_argument, 		0, 'r'},
		{"mirror", 			no_argument, 		0, 'm'},
        {"cycle-time",		required_argument,	0, 'c'},
        {"burst-length",	required_argument,	0, 'b'},
		// Optional options
		{"dest-mac", 		required_argument,  0, 'd'},
		{"log-file", 		required_argument,	0, 'l'},
		{"mqtt", 			no_argument, 		0, 'M'},
        {"deltas", 			no_argument, 		0, 'D'},
        {"link-speed",  	required_argument,	0, 's'},
        {"run-duration", 	required_argument,  0, 'R'},
        {"promiscuous", 	no_argument, 		0, 'P'},
        {"mirror-serial", 	no_argument, 		0, 'S'},
		{"internal-debug", 	no_argument, 		0, 'i'},
        {"help",			no_argument, 		0, 'h'},
		// long options only
		{"skip-count", 		required_argument,  0, OPT_SKIP_NUM},
		{"delay-time", 		required_argument,	0, OPT_DELAY_NUM},
        {"continue-on-err", no_argument, 		0, OPT_CONTINUE_ON_ERROR_NUM},
#if HAS_HW_TIMESTAMPING
        {"hw-timestamp", 	no_argument, 		0, OPT_HW_TIMESTAMP_NUM},
#endif
        {"workload",     no_argument,        0, 'w'},
		{NULL, 0, 0, 0}
	};
	const char *short_options = "rmc:b:d:l:MDs:R:PSihw:";
    // clang-format on
    argvopt = argv;

    pinfo->delay_sec        = DEFAULT_DELAY_SEC;
    pinfo->link_speed       = RTE_ETH_SPEED_NUM_UNKNOWN;
    pinfo->pkt_skip_cnt     = DEFAULT_SKIP_COUNT;
    pinfo->run_duration_str = strdup("000:00:00");
    pinfo->dest_mac_str     = strdup("FF:FF:FF:FF:FF:FF");

    // Parse the command line options.
    while ((opt = getopt_long(argc, argvopt, short_options, lgopts, &option_index)) != EOF) {

        switch (opt) {
        case 'r':        // Reference mode (mirror mode == false default)
            pinfo->mirror_enabled = false;
            break;
        case 'm':        // mirror mode (reference mode == false default)
            pinfo->mirror_enabled = true;
            break;
        case 'c':        // cycle-time
            pinfo->cycle_time_ns = strtoul(optarg, NULL, 0);
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
            break;
        case 'd':        // dest-mac
            free(pinfo->dest_mac_str);
            pinfo->dest_mac_str = strdup(optarg);
            break;
        case 'l':        // log-file
            if (!pinfo->log_enabled && log_init(optarg))
                rte_exit(EXIT_FAILURE, "Error: Failed to open log file\n");
            pinfo->log_enabled = true;
            break;
        case 'M':        // MQTT enabled
            if (!pinfo->mqtt_enabled && mqtt_init())
                rte_exit(EXIT_FAILURE, "Error: Failed to initialize MQTT\n");
            pinfo->mqtt_enabled = true;
            break;
        case 'D':        // log all time deltas
            pinfo->deltas_enabled = true;
            break;
        case 's':        // link speed Mbps
            pinfo->link_speed = strtoul(optarg, NULL, 0);
            break;
        case 'R':        // Run duration in seconds
            process_duration(optarg);
            break;
        case 'P':        // promiscuous mode
            pinfo->promiscuous_mode = true;
            break;
        case 'S':        // mirror serial mode
            pinfo->mirror_serial_enabled = true;
            break;
		case 'i':        // internal debug mode
		    pinfo->internal_debug_enabled = true;
			break;
        case 'h':
            print_usage(prgname);
            break;

        case OPT_SKIP_NUM:        // Number of packets to skip
            pinfo->pkt_skip_cnt = atoi(optarg);
            break;
        case OPT_DELAY_NUM:        // Delay start TX in seconds
            pinfo->delay_sec = atoi(optarg);
            break;
        case OPT_CONTINUE_ON_ERROR_NUM:        // Continue running even if errors occur
            pinfo->continue_on_error = true;
            break;
#if HAS_HW_TIMESTAMPING
        case OPT_HW_TIMESTAMP_NUM:        // hardware timestamp mode
            pinfo->hw_timestamp_enabled = true;
            break;
#endif
        case 'w':
            // TO-DO: How do we indicate that workload requires -S ?
            process_workload(optarg);
            break;

        default:
            print_usage(prgname);
            break;
        }
    }

    if ((pinfo->rt_workload.enabled) && !pinfo->mirror_serial_enabled)
        rte_exit(EXIT_FAILURE,
            "Error: Invalid arguments, enabling a workload requires the "
                "application to be in mirror (-m) mode.\n");

    if (pinfo->rt_workload.enabled && !pinfo->mirror_enabled)
        rte_exit(EXIT_FAILURE,
            "Error: Invalid arguments, enabling a workload requires mirror to "
                "be configured in Serialized (-S) mode.\n");

    if (pinfo->cycle_time_ns == 0 || pinfo->burst_count == 0 || pinfo->pkt_length == 0)
        rte_exit(EXIT_FAILURE,
                 "Error: Invalid arguments, must contain cycle time and Burst/Length\n");

    argv[optind - 1] = prgname;

    optind = 1; /* Reset getopt lib. */

    return 0;
}

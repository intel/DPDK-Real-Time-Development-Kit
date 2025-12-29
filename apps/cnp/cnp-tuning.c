/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"

static info_t info = {0};
info_t *pinfo      = &info;

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        stop_running();
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
    uint16_t nb_ports, nb_lcores, port_id;
    int ret;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    setlocale(LC_ALL, "");        // Allow for formatted numbers .e.g, 1,000,000

    /* Register application usage hook before EAL init */
    rte_set_application_usage_hook(print_app_usage);

    /* Initialize the Environment Abstraction Layer (EAL). */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    // make sure we have at least 1 extra lcore for main loop processing for stats and keyboard
    nb_lcores = rte_lcore_count();
    if (nb_lcores < 2)
        rte_exit(EXIT_FAILURE, "Too few lcores available. Number of lcore needed is %u.\n", 2);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No available Ethernet ports - bye\n");
    if (nb_ports > 1)
        printf("Number of ports: %u, using only first one\n", nb_ports);

    ret = parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with initialization\n");

    printf("Starting packet %s application\n", "CNP Tuning");
    sleep_sec(1);

    start_running();

    /* Launch worker thread on worker lcores only */
    if (rte_eal_mp_remote_launch(rxtx_routine, NULL, SKIP_MAIN) < 0)
        rte_exit(EXIT_FAILURE, "Cannot launch lcores\n");

    sleep_sec(1);        // Give some time for worker lcores to start

    for (int n = 0; n < 64; n++)
        printf("\n");
    keyboard_loop();

    RTE_ETH_FOREACH_DEV(port_id)
    {
        int ret;

        if ((ret = rte_eth_dev_stop(port_id)) < 0)
            printf("rte_eth_dev_stop: err=%d, port=%d, %s\n", ret, port_id, rte_strerror(ret));

        rte_eth_dev_close(port_id);
    }

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}

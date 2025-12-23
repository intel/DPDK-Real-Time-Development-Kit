/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */

#include "cnp-tuning.h"
#include "log.h"
#include "mqtt.h"

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
    uint16_t nb_ports, nb_lcores, port_id, nb_lcores_needed;
    int ret, main_lcore_id;

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

    ret = parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with initialization\n");

    nb_lcores_needed = (_btst(MQTT)) ? 3 : 2;

    // make sure we have at least 1 extra lcore for main loop processing for stats and keyboard
    nb_lcores = rte_lcore_count();
    if (nb_lcores < nb_lcores_needed)
        rte_exit(EXIT_FAILURE, "Too few lcores available. Number of lcore needed is %u.\n",
                 nb_lcores_needed);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No available Ethernet ports - bye\n");
    if (nb_ports > 1)
        printf("Number of ports: %u, using only first one\n", nb_ports);

    main_lcore_id = rte_get_main_lcore();
    if (_btst(MQTT))
        pinfo->mqtt_lcore_id = ++main_lcore_id;
    pinfo->worker_lcore_id = ++main_lcore_id;

    // Assign each worker lcore a port and initialize its data structure.
    lcore_t *lcore = &pinfo->lcores[pinfo->worker_lcore_id];

    lcore->lport.lcore_id = pinfo->worker_lcore_id;
    lcore->lport.pid      = 0;
    lcore->lport.qid      = 0;

    lcore->rx_mbufs = rte_zmalloc_socket("RX mbufs", MAX_BURST_COUNT * sizeof(struct rte_mbuf *), 0,
                                         rte_socket_id());
    if (lcore->rx_mbufs == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for RX mbufs\n");

    lcore->tx_mbufs = rte_zmalloc_socket("TX mbufs", MAX_BURST_COUNT * sizeof(struct rte_mbuf *), 0,
                                         rte_socket_id());
    if (lcore->tx_mbufs == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate memory for TX mbufs\n");

    if (port_init(&lcore->lport) < 0)
        rte_exit(EXIT_FAILURE, "Cannot init lport %u:%u\n", lcore->lport.pid, lcore->lport.qid);

    for (int n = 0; n < 64; n++)
        printf("\n");
    printf("Starting packet %s application\n", "Launch-Time");
    sleep_sec(1);

    start_running();

    if (_btst(MQTT)) {
        /* Launch MQTT thread on lcore */
        if (rte_eal_remote_launch(mqtt_thread_routine, &pinfo->lcores[pinfo->mqtt_lcore_id],
                                  pinfo->mqtt_lcore_id) < 0)
            rte_exit(EXIT_FAILURE, "Cannot launch lcore %u\n", pinfo->mqtt_lcore_id);
    }
    /* Launch worker thread on lcore */
    if (rte_eal_remote_launch(rxtx_routine,
                              &pinfo->lcores[pinfo->worker_lcore_id], pinfo->worker_lcore_id) < 0)
        rte_exit(EXIT_FAILURE, "Cannot launch lcore %u\n", pinfo->worker_lcore_id);

    keyboard_loop();

    RTE_ETH_FOREACH_DEV(port_id)
    {
        int ret;

        if ((ret = rte_eth_dev_stop(port_id)) < 0)
            printf("rte_eth_dev_stop: err=%d, port=%d, %s\n", ret, port_id, rte_strerror(ret));

        rte_eth_dev_close(port_id);
    }
    log_flush();

    rte_free(lcore->rx_mbufs);
    rte_free(lcore->tx_mbufs);

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}

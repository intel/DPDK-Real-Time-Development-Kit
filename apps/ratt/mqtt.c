/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */
#include "log.h"
#include "mqtt.h"

typedef struct mqtt_info {
    struct rte_ring *ring;                     // Log ring for statistics
    struct rte_mempool *pool;                  // Memory pool for log ring
    struct rte_ring *delta_ring;               // Log ring for deltas
    struct mosquitto *mosq;                    // MQTT client
    void *buffs[LOG_MQTT_BUFFER_COUNT];        // MQTT buffers
    char *message;                             // MQTT message
} mqtt_info_t;

#define MQTT_M2_TOPIC            "m2-data"
#define MQTT_MEASURE_NAME_DELTAS "m2-deltas"
#define MQTT_MEASURE_NAME_STATS  "m2-stats"

static mqtt_info_t mqtt_info = {0};
mqtt_info_t *mqtt            = &mqtt_info;

void
mqtt_delta(uint64_t delta)
{
    if (!pinfo->deltas_enabled || mqtt->delta_ring == NULL)
        return;

    if (rte_ring_enqueue(mqtt->delta_ring, (void *)delta))
        fprintf(stderr, "Failed to enqueue MQTT delta statistic to ring\n");
}

void
mqtt_stats(stats_t *stats, struct rte_eth_stats *port_stats)
{
    struct mqtt_statistics *internal;
    int ret;

    if(stats->snapshot.max_ns == 0)
	    return;

    if (!pinfo->mqtt_enabled || mqtt->ring == NULL || mqtt->pool == NULL)
        return;
    ret = rte_mempool_get(mqtt->pool, (void **)&internal);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate memory for MQTT log statistics\n");
        return;
    }

    internal->timestamp = clock_get_ns();
    rte_memcpy(&internal->stats, stats, sizeof(*stats));
    rte_memcpy(&internal->port_stats, port_stats, sizeof(*port_stats));

    if (rte_ring_enqueue(mqtt->ring, (void *)internal))
        fprintf(stderr, "Failed to enqueue MQTT log statistics to ring\n");
}

/*
{
    "reference": {
        "Timestamp": 1746903329812009200,
        "MeasurementName": "m2-deltas",
        "stats": {
            "deltas": [
                282083,
                281614,
                281668,
                281636
            ]
        }
    }
}
*/
int
mqtt_add_deltas(struct mosquitto *mosq, const char *mqtt_measurement_name)
{
    int ret = 0;

    if (!pinfo->deltas_enabled)
        return ret;

    while (rte_ring_count(mqtt->delta_ring) > 0) {
        size_t message_length;
        int written, result_pub;
        char *p;

        // The rte_ring_count() test above means we have entries in the ring.
        uint32_t nb_deltas =
            rte_ring_dequeue_burst(mqtt->delta_ring, mqtt->buffs, LOG_MQTT_BUFFER_COUNT, NULL);

        mqtt->message[0] = '\0';
        message_length   = LOG_MQTT_BUFFER_SIZE - 1;
        p                = mqtt->message;

        written = snprintf(p, message_length,
                           "{\n\t\"%s\": {\n"
                           "\t\t\"Timestamp\": %" PRIu64 ",\n"
                           "\t\t\"MeasurementName\": \"%s\",\n",
                           "reference", clock_get_ns(), mqtt_measurement_name);
        p += written;
        message_length -= written;

        written =
            snprintf(p, message_length, "\t\t\"%s\": {\n\t\t\"info\": \"dtag\",\n\t\t\t\"%s\": [\n", "stats", "deltas");
        p += written;
        message_length -= written;

        for (uint32_t i = 0; i < nb_deltas; i++) {
            uint64_t delta_ns = (uint64_t)mqtt->buffs[i];

            written = snprintf(p, message_length, "\t\t\t\t%" PRIu64 "%s", delta_ns,
                               ((i + 1) < nb_deltas) ? ",\n" : "\n");
            p += written;
            message_length -= written;
        }
        written = snprintf(p, message_length, "\t\t\t]\n\t\t}\n\t}\n}\n");
        p += written;
        message_length -= written;

        result_pub = mosquitto_publish(mosq, NULL, MQTT_M2_TOPIC, strlen(mqtt->message),
                                       mqtt->message, 2, false);
        if (result_pub != MOSQ_ERR_SUCCESS)
            fprintf(stderr, "Error delta publishing: %s\n", mosquitto_strerror(result_pub));
    }
    return ret;
}

static void
append_mqtt_stats(char **message, size_t *message_length, const char *name, uint64_t value)
{
    int written;

    written = snprintf(*message, *message_length, "\t\t\t\"%s\" : %" PRIu64 ",\n", name, value);

    *message += written;
    *message_length -= written;
}

static int
mqtt_add_stats(struct mosquitto *mosq, const char *mqtt_measurement_name)
{
    stats_t *stats;
    struct rte_eth_stats *port;
    size_t message_length;
    int written, result_pub, nb_buffs;
    char *p;

    if (!pinfo->mqtt_enabled || mqtt->ring == NULL || mqtt->pool == NULL)
        return 0;

    nb_buffs =
        rte_ring_dequeue_burst(mqtt->ring, (void **)mqtt->buffs, LOG_MQTT_BUFFER_COUNT, NULL);
    if (nb_buffs == 0)
        return 0;

    for (int i = 0; i < nb_buffs; i++) {
        mqtt_statistics_t *mqtt_stats = mqtt->buffs[i];

        stats = &mqtt_stats->stats;
        port  = &mqtt_stats->port_stats;

        p              = mqtt->message;
        p[0]           = '\0';
        message_length = LOG_MQTT_BUFFER_SIZE - 1;

        written = snprintf(p, message_length,
                           "{\n"
                           "\t\"reference\": {\n"
                           "\t\t\"Timestamp\":%" PRIu64 ",\n"
                           "\t\t\"MeasurementName\":\"%s\",\n"
                           "\t\t\"stats\": {\n",
                           mqtt_stats->timestamp, mqtt_measurement_name);
        p += written;
        message_length -= written;

        append_mqtt_stats(&p, &message_length, "rttMinNs", stats->rtt.min_ns);
        append_mqtt_stats(&p, &message_length, "rttMaxNs", stats->rtt.max_ns);
        append_mqtt_stats(&p, &message_length, "rttSumNs", stats->rtt.sum_ns);
        append_mqtt_stats(&p, &message_length, "rttCount", stats->rtt.count);
        append_mqtt_stats(&p, &message_length, "rttAvgNs", stats->rtt.avg_ns);

        append_mqtt_stats(&p, &message_length, "snapRttMinNs", stats->snapshot.min_ns);
        append_mqtt_stats(&p, &message_length, "snapRttMaxNs", stats->snapshot.max_ns);
        append_mqtt_stats(&p, &message_length, "snapRttSumNs", stats->snapshot.sum_ns);
        append_mqtt_stats(&p, &message_length, "snapRttCount", stats->snapshot.count);
        append_mqtt_stats(&p, &message_length, "snapRttAvgNs", stats->snapshot.avg_ns);

        append_mqtt_stats(&p, &message_length, "spikeRttMinNs", stats->spike.min_ns);
        append_mqtt_stats(&p, &message_length, "spikeRttMaxNs", stats->spike.max_ns);
        append_mqtt_stats(&p, &message_length, "spikeRttSumNs", stats->spike.sum_ns);
        append_mqtt_stats(&p, &message_length, "spikeRttCount", stats->spike.count);
        append_mqtt_stats(&p, &message_length, "spikeRttAvgNs", stats->spike.avg_ns);

        append_mqtt_stats(&p, &message_length, "rxSnapshotMinNs", stats->rx_snapshot.min_ns);
        append_mqtt_stats(&p, &message_length, "rxSnapshotMaxNs", stats->rx_snapshot.max_ns);
        append_mqtt_stats(&p, &message_length, "rxSnapshotSumNs", stats->rx_snapshot.sum_ns);
        append_mqtt_stats(&p, &message_length, "rxSnapshotCount", stats->rx_snapshot.count);
        append_mqtt_stats(&p, &message_length, "rxSnapshotAvgNs", stats->rx_snapshot.avg_ns);

        append_mqtt_stats(&p, &message_length, "txSnapshotMinNs", stats->tx_snapshot.min_ns);
        append_mqtt_stats(&p, &message_length, "txSnapshotMaxNs", stats->tx_snapshot.max_ns);
        append_mqtt_stats(&p, &message_length, "txSnapshotSumNs", stats->tx_snapshot.sum_ns);
        append_mqtt_stats(&p, &message_length, "txSnapshotCount", stats->tx_snapshot.count);
        append_mqtt_stats(&p, &message_length, "txSnapshotAvgNs", stats->tx_snapshot.avg_ns);

        append_mqtt_stats(&p, &message_length, "rxPPS", stats->rx_pps);
        append_mqtt_stats(&p, &message_length, "txPPS", stats->tx_pps);
        append_mqtt_stats(&p, &message_length, "noMBufs", stats->no_mbufs);
        append_mqtt_stats(&p, &message_length, "noTimestamp", stats->no_timestamp);

        append_mqtt_stats(&p, &message_length, "idError", stats->id_error);
        append_mqtt_stats(&p, &message_length, "txRingFull", stats->tx_ring_full);
        append_mqtt_stats(&p, &message_length, "rxTimeout", stats->rx_timeout);

		append_mqtt_stats(&p, &message_length, "rxTryExtraTime", stats->rx_try_extra_time);

        append_mqtt_stats(&p, &message_length, "totalPktsRx", stats->total_pkts.rx);
        append_mqtt_stats(&p, &message_length, "totalPktsTx", stats->total_pkts.tx);

        append_mqtt_stats(&p, &message_length, "ipackets", port->ipackets);
        append_mqtt_stats(&p, &message_length, "opackets", port->opackets);
        append_mqtt_stats(&p, &message_length, "ibytes", port->ibytes);
        append_mqtt_stats(&p, &message_length, "obytes", port->obytes);
        append_mqtt_stats(&p, &message_length, "imissed", port->imissed);
        append_mqtt_stats(&p, &message_length, "ierrors", port->ierrors);
        append_mqtt_stats(&p, &message_length, "oerrors", port->oerrors);
        append_mqtt_stats(&p, &message_length, "rxNoMbuf", port->rx_nombuf);

        // remove last comma and newline (since the last element in the array can't have a comma)
        // and add the newline below
        p -= 2;
        message_length += written;

        // included first newline here last element above
        written = snprintf(p, message_length, "\n\t\t}\t\t\n\t}\n}\n");
        p += written;
        message_length -= written;

        log_message("stats:%s\n", mqtt->message);
        result_pub = mosquitto_publish(mosq, NULL, MQTT_M2_TOPIC, strlen(mqtt->message),
                                       mqtt->message, 2, false);
        if (result_pub != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(result_pub));
            return -1;
        }
    }
    rte_mempool_put_bulk(mqtt->pool, mqtt->buffs, nb_buffs);

    return 0;
}

static void
mqtt_on_connect(struct mosquitto *mosq, void *obj __rte_unused, int reason_code)
{
    if (reason_code != 0)
        mosquitto_disconnect(mosq);
}

int
mqtt_thread_routine(void *data __rte_unused)
{
    uint64_t period_ns = LOG_MQTT_PERIOD_NS;
    int ret, connect_status;
    struct timespec time;

    mosquitto_lib_init();

    mqtt->mosq = mosquitto_new(NULL, true, NULL);
    if (mqtt->mosq == NULL) {
        fprintf(stderr, "*** MQTT Error: Out of memory.\n");
        goto err_mqtt_outof_memory;
    }

    connect_status = mosquitto_connect(mqtt->mosq, "127.0.0.1", 1883, 60);
    if (connect_status != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "*** MQTT Error by connect: %s\n", mosquitto_strerror(connect_status));
        goto err_mqtt_connect;
    }

    mosquitto_connect_callback_set(mqtt->mosq, mqtt_on_connect);

    ret = mosquitto_loop_start(mqtt->mosq);
    if (ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "*** MQTT Error: %s\n", mosquitto_strerror(ret));
        goto err_mqtt_start;
    }

    /*
     * Send the statistics periodically to the MQTT broker.  This thread can run with low
     * priority to not influence to Application Tasks that much.
     */
    ret = clock_gettime(CLOCK_TAI, &time);
    if (ret) {
        fprintf(stderr, "*** MQTT: clock_gettime() failed: %s!", strerror(errno));
        goto err_time;
    }

    while (is_running()) {
        increment_period(&time, period_ns);
        ret = clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &time, NULL);
        if (ret) {
            printf("*** MQTT: clock_nanosleep() failed");
            goto err_time;
        }

        if (mqtt_add_stats(mqtt->mosq, MQTT_MEASURE_NAME_STATS)) {
            fprintf(stderr, "*** Error processing MQTT statistics\n");
            break;
        }

        if (mqtt_add_deltas(mqtt->mosq, MQTT_MEASURE_NAME_DELTAS)) {
            fprintf(stderr, "*** Error processing MQTT delta statistics\n");
            break;
        }
    }
    return 0;

err_mqtt_outof_memory:
err_mqtt_connect:
err_mqtt_start:
err_time:
    mqtt_close();
    return -1;
}

int
mqtt_init(void)
{
    int socket_id = rte_socket_id();
    uint32_t buff_sz;

    // Setup pool and ring for basic stats
    mqtt->ring =
        rte_ring_create("mqtt", LOG_RING_SIZE, socket_id, RING_F_SC_DEQ | RING_F_MP_RTS_ENQ);
    if (!mqtt->ring) {
        printf("Error: Could not create MQTT log ring\n");
        return -ENOMEM;
    }

    buff_sz    = RTE_ALIGN_CEIL(sizeof(mqtt_statistics_t), RTE_CACHE_LINE_SIZE);
    mqtt->pool = rte_mempool_create("mqtt_pool", LOG_MQTT_BUFFER_COUNT, buff_sz, 64,
                                    DEFAULT_PRIV_SIZE, NULL, NULL, NULL, NULL, socket_id, 0);
    if (!mqtt->pool) {
        printf("Error: Could not create MQTT log pool\n");
        return -ENOMEM;
    }

    uint32_t ring_size = pow2roundup(((NS_PER_S / pinfo->cycle_time_ns) * 2));

    mqtt->delta_ring =
        rte_ring_create("mdelta", ring_size, socket_id, RING_F_MC_RTS_DEQ | RING_F_MP_RTS_ENQ);
    if (!mqtt->delta_ring) {
        printf("Error: Could not create MQTT delta ring of size %'u entries\n", ring_size);
        return -ENOMEM;
    }

    mqtt->message = calloc(LOG_MQTT_BUFFER_SIZE, sizeof(char));
    if (!mqtt->message) {
        printf("Error: Could not allocate memory for MQTT stats message\n");
        return -ENOMEM;
    }

    return 0;
}

void
mqtt_close(void)
{
    if (pinfo->mqtt_enabled) {
        sleep_nsec(LOG_MQTT_PERIOD_NS * 2UL);        // wait a bit for a flush before closing
        if (mqtt->mosq)
            mosquitto_destroy(mqtt->mosq);
        mosquitto_lib_cleanup();

        rte_ring_free(mqtt->ring);
        rte_mempool_free(mqtt->pool);
        rte_ring_free(mqtt->delta_ring);

        free(mqtt->message);
		memset(mqtt, 0, sizeof(mqtt_info_t));
    }
}

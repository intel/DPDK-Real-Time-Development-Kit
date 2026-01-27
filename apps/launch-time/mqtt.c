/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */
#include "launch-time.h"

#ifdef DISABLE_MQTT

int mqtt_init(void) { return -1; }
void mqtt_stats(const stats_t *stats __rte_unused, const struct rte_eth_stats *port_stats __rte_unused) { }
void mqtt_close(void) { }
int mqtt_thread_routine(void *data __rte_unused) { return 0; }

#else

#include "log.h"
#include "mqtt.h"

typedef struct mqtt_info {
    struct rte_ring *ring;                     // Log ring for statistics
    struct rte_mempool *pool;                  // Memory pool for log ring
    struct mosquitto *mosq;                    // MQTT client
    void *buffs[LOG_MQTT_BUFFER_COUNT];        // MQTT buffers
    char *message;                             // MQTT message
} mqtt_info_t;

#define MQTT_M2_TOPIC            "lttt-data"
#define MQTT_MEASURE_NAME_STATS  "lttt-stats"

static mqtt_info_t mqtt_info = {0};
mqtt_info_t *mqtt            = &mqtt_info;

/* Clamp snprintf return to prevent size_t underflow on message_length */
static inline int
clamp_written(int written, size_t avail)
{
    if (written < 0)
        return 0;
    return ((size_t)written > avail) ? (int)avail : written;
}

void
mqtt_stats(const stats_t *stats, const struct rte_eth_stats *port_stats)
{
    struct mqtt_statistics *internal;
    int ret;

    if (!_btst(MQTT) || mqtt->ring == NULL || mqtt->pool == NULL)
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

static void
append_mqtt_stats(char **message, size_t *message_length, const char *name, uint64_t value)
{
    int written;

    written = snprintf(*message, *message_length, "\t\t\t\"%s\" : %" PRIu64 ",\n", name, value);
    if (written < 0)
        return;
    if ((size_t)written > *message_length)
        written = *message_length;

    *message += written;
    *message_length -= written;
}

static int
mqtt_add_stats(struct mosquitto *mosq, const char *mqtt_measurement_name)
{
    const stats_t *stats;
    const struct rte_eth_stats *port;
    size_t message_length;
    int written, result_pub, nb_buffs;
    char *p;

    if (!_btst(MQTT) || mqtt->ring == NULL || mqtt->pool == NULL)
        return 0;

    nb_buffs =
        rte_ring_dequeue_burst(mqtt->ring, (void **)mqtt->buffs, LOG_MQTT_BUFFER_COUNT, NULL);
    if (nb_buffs == 0)
        return 0;

    for (int i = 0; i < nb_buffs; i++) {
        mqtt_statistics_t *mstats = mqtt->buffs[i];

        stats = &mstats->stats;
        port  = &mstats->port_stats;

        p              = mqtt->message;
        p[0]           = '\0';
        message_length = LOG_MQTT_BUFFER_SIZE - 1;

        written = snprintf(p, message_length,
                           "{\n"
                           "\t\"reference\": {\n"
                           "\t\t\"Timestamp\":%" PRIu64 ",\n"
                           "\t\t\"MeasurementName\":\"%s\",\n"
                           "\t\t\"stats\": {\n",
                           mstats->timestamp, mqtt_measurement_name);
        written = clamp_written(written, message_length);
        p += written;
        message_length -= written;

        append_mqtt_stats(&p, &message_length, "rxPPS", stats->rx_pps);
        append_mqtt_stats(&p, &message_length, "txPPS", stats->tx_pps);
        append_mqtt_stats(&p, &message_length, "noMBufs", stats->no_mbufs);

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
        message_length += 2;

        // included first newline here last element above
        written = snprintf(p, message_length, "\n\t\t}\t\t\n\t}\n}\n");
        written = clamp_written(written, message_length);
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
        goto err_pool;
    }

    mqtt->message = calloc(LOG_MQTT_BUFFER_SIZE, sizeof(char));
    if (!mqtt->message) {
        printf("Error: Could not allocate memory for MQTT stats message\n");
        goto err_message;
    }

    return 0;

err_message:
    rte_mempool_free(mqtt->pool);
    mqtt->pool = NULL;
err_pool:
    rte_ring_free(mqtt->ring);
    mqtt->ring = NULL;
    return -ENOMEM;
}

void
mqtt_close(void)
{
    if (_btst(MQTT)) {
        _bclr(MQTT);
        sleep_nsec(LOG_MQTT_PERIOD_NS * 2UL);        // wait a bit for a flush before closing
        if (mqtt->mosq) {
            mosquitto_disconnect(mqtt->mosq);
            mosquitto_loop_stop(mqtt->mosq, true);
            mosquitto_destroy(mqtt->mosq);
        }
        mosquitto_lib_cleanup();

        rte_ring_free(mqtt->ring);
        rte_mempool_free(mqtt->pool);

        free(mqtt->message);
        memset(mqtt, 0, sizeof(mqtt_info_t));
    }
}

#endif /* DISABLE_MQTT */

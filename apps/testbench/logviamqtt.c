// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2024 Intel Corporation.
 * Author Walfred Tedeschi <walfred.tedeschi@intel.com>
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rte_common.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "app_config.h"
#ifdef WITH_MQTT
#include <mosquitto.h>
#endif

#include "config.h"
#include "functions.h"
#include "log.h"
#include "logviamqtt.h"
#include "stat.h"
#include "thread.h"
#include "utils.h"

#ifndef WITH_MQTT
void
log_via_mqtt_stats(enum stat_frame_type frame_type __rte_unused,
                   struct statistics *stats __rte_unused)
{
}

#else
struct log_via_mqtt_thread_context mqtt_context = {0};

struct log_statistics {
    enum stat_frame_type frame_type;
    uint64_t time_stamp;
    uint64_t frames_sent;
    uint64_t frames_received;
    uint64_t out_of_order_errors;
    uint64_t frame_id_errors;
    uint64_t payload_errors;
    uint64_t round_trip_min;
    uint64_t round_trip_max;
    uint64_t round_trip_outliers;
    uint64_t oneway_min;
    uint64_t oneway_max;
    uint64_t oneway_outliers;
    double round_trip_avg;
    double oneway_avg;
};

static int
log_via_mqtt_init(void)
{
    int socket_id = rte_socket_id();

    mqtt_context.mqtt_ring = rte_ring_create("logmqtt", LOG_MQTT_RING_SIZE, socket_id,
                                             RING_F_SC_DEQ | RING_F_MP_RTS_ENQ);
    if (!mqtt_context.mqtt_ring)
        return -ENOMEM;

    mqtt_context.mqtt_pool = rte_mempool_create("logmqtt_pool", LOG_MQTT_MEMPOOL_BUFFER_COUNT,
                                                LOG_MQTT_MEMPOOL_BUFFER_SIZE, 64, 0, NULL, NULL,
                                                NULL, NULL, socket_id, 0);
    if (!mqtt_context.mqtt_pool) {
        rte_ring_free(mqtt_context.mqtt_ring);
        mqtt_context.mqtt_ring = NULL;
        return -ENOMEM;
    }

    return 0;
}

void
log_via_mqtt_stats(enum stat_frame_type frame_type, struct statistics *stats)
{
    struct log_statistics *internal;
    int ret;

    if (app_config.log_via_mqtt == false)
        return;

    ret = rte_mempool_get(mqtt_context.mqtt_pool, (void **)&internal);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate memory for log statistics\n");
        return;
    }

    internal->frame_type          = frame_type;
    internal->time_stamp          = stats->last_time_stamp;
    internal->frames_sent         = stats->frames_sent;
    internal->frames_received     = stats->frames_received;
    internal->out_of_order_errors = stats->out_of_order_errors;
    internal->frame_id_errors     = stats->frame_id_errors;
    internal->payload_errors      = stats->payload_errors;

    internal->round_trip_min      = stats->round_trip_min;
    internal->round_trip_max      = stats->round_trip_max;
    internal->round_trip_outliers = stats->round_trip_outliers;
    internal->round_trip_avg      = stats->round_trip_avg;

    internal->oneway_min      = stats->oneway_min;
    internal->oneway_max      = stats->oneway_max;
    internal->oneway_outliers = stats->oneway_outliers;
    internal->oneway_avg      = stats->oneway_avg;

    if (rte_ring_enqueue(mqtt_context.mqtt_ring, (void *)internal))
        fprintf(stderr, "Failed to enqueue log statistics to ring\n");
}

static void
log_via_mqtt_add_traffic_class(struct mosquitto *mosq, const char *mqtt_base_topic_name,
                               struct log_statistics *stat)
{
    char stat_message[2048] = {}, *p;
    size_t stat_message_length;
    int written, result_pub;
    uint64_t time_ns;

    stat_message_length = sizeof(stat_message) - 1;
    p                   = stat_message;

    time_ns = stat->time_stamp;
    written = snprintf(p, stat_message_length,
                       "{\"%s\" :\n"
                       "\t{\"Timestamp\" : %" PRIu64 ",\n"
                       "\t \"MeasurementName\" : \"%s\"",
                       "reference", time_ns, mqtt_base_topic_name);
    if (written > 0 && (size_t)written < stat_message_length) {
        p += written;
        stat_message_length -= written;
    } else if (written > 0) {
        p += stat_message_length;
        stat_message_length = 0;
    }

    written = snprintf(p, stat_message_length,
                       ",\n\t\t\"%s\" : \n\t\t{\n"
                       "\t\t\t\"TCName\" : \"%s\",\n"
                       "\t\t\t\"FramesSent\" : %" PRIu64 ",\n"
                       "\t\t\t\"FramesReceived\" : %" PRIu64 ",\n"
                       "\t\t\t\"RoundTripTimeMin\" : %" PRIu64 ",\n"
                       "\t\t\t\"RoundTripMax\" : %" PRIu64 ",\n"
                       "\t\t\t\"RoundTripAv\" : %lf,\n"
                       "\t\t\t\"OnewayMin\" : %" PRIu64 ",\n"
                       "\t\t\t\"OnewayMax\" : %" PRIu64 ",\n"
                       "\t\t\t\"OnewayAv\" : %lf,\n"
                       "\t\t\t\"OutofOrderErrors\" : %" PRIu64 ",\n"
                       "\t\t\t\"FrameIdErrors\" : %" PRIu64 ",\n"
                       "\t\t\t\"PayloadErrors\" : %" PRIu64 ",\n"
                       "\t\t\t\"RoundTripOutliers\" : %" PRIu64 ",\n"
                       "\t\t\t\"OnewayOutliers\" : %" PRIu64 "\n\t\t}",
                       "stats", stat_frame_type_to_string(stat->frame_type), stat->frames_sent,
                       stat->frames_received, stat->round_trip_min, stat->round_trip_max,
                       stat->round_trip_avg, stat->oneway_min, stat->oneway_max, stat->oneway_avg,
                       stat->out_of_order_errors, stat->frame_id_errors, stat->payload_errors,
                       stat->round_trip_outliers, stat->oneway_outliers);
    if (written > 0 && (size_t)written < stat_message_length) {
        p += written;
        stat_message_length -= written;
    } else if (written > 0) {
        p += stat_message_length;
        stat_message_length = 0;
    }

    written = snprintf(p, stat_message_length, "\t\t\n}\t\n}\n");
    if (written > 0 && (size_t)written < stat_message_length) {
        p += written;
        stat_message_length -= written;
    }

    result_pub =
        mosquitto_publish(mosq, NULL, "testbench", strlen(stat_message), stat_message, 2, false);
    if (result_pub != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(result_pub));
}

static void
log_via_mqtt_on_connect(struct mosquitto *mosq, void *obj __rte_unused, int reason_code)
{
    if (reason_code != 0)
        mosquitto_disconnect(mosq);
}

static int
log_via_mqtt_thread_routine(void *data __rte_unused)
{
    struct thread_context *ctx = data;
    uint64_t period_ns         = app_config.log_via_mqtt_thread_period_ns;
    int ret, connect_status;
    struct timespec time;

    mosquitto_lib_init();

    mqtt_context.mosq = mosquitto_new(NULL, true, NULL);
    if (mqtt_context.mosq == NULL) {
        fprintf(stderr, "MQTTLog Error: Out of memory.\n");
        goto err_mqtt_outof_memory;
    }

    connect_status = mosquitto_connect(mqtt_context.mosq, app_config.log_via_mqtt_broker_ip,
                                       app_config.log_via_mqtt_broker_port,
                                       app_config.log_via_mqtt_keep_alive_secs);
    if (connect_status != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTTLog Error by connect: %s\n", mosquitto_strerror(connect_status));
        goto err_mqtt_connect;
    }

    mosquitto_connect_callback_set(mqtt_context.mosq, log_via_mqtt_on_connect);

    ret = mosquitto_loop_start(mqtt_context.mosq);
    if (ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Log Via MQTT Error: %s\n", mosquitto_strerror(ret));
        goto err_mqtt_start;
    }

    /*
     * Send the statistics periodically to the MQTT broker.  This thread can run with low
     * priority to not influence to Application Tasks that much.
     */
    ret = clock_gettime(app_config.application_clock_id, &time);
    if (ret) {
        fprintf(stderr, "Log Via MQTT: clock_gettime() failed: %s!", strerror(errno));
        goto err_time;
    }

    while (!ctx->stop) {
        char *buffer[LOG_MQTT_BUFFER_COUNT];

        increment_period(&time, period_ns);
        do {
            ret = clock_nanosleep(app_config.application_clock_id, TIMER_ABSTIME, &time, NULL);
        } while (ret == EINTR);

        for (;;) {
            int n = rte_ring_dequeue_burst(mqtt_context.mqtt_ring, (void **)&buffer,
                                           LOG_MQTT_BUFFER_COUNT, NULL);
            if (n == 0)
                break;

            for (int i = 0; i < n; i++) {
                struct log_statistics *stats = (struct log_statistics *)buffer[i];

                log_via_mqtt_add_traffic_class(mqtt_context.mosq,
                                               app_config.log_via_mqtt_measurement_name, stats);
            }
            rte_mempool_put_bulk(mqtt_context.mqtt_pool, (void *const *)buffer, n);
        }
    }
    fprintf(stderr, "Log Via MQTT Thread stopped\n");

    return 0;

err_mqtt_outof_memory:
err_mqtt_connect:
err_mqtt_start:
err_time:
    if (mqtt_context.mosq)
        mosquitto_destroy(mqtt_context.mosq);
    mosquitto_lib_cleanup();
    return -1;
}

static int
log_via_mqtt_thread_create(struct thread_context *thread_context __rte_unused)
{
    if (!app_config.log_via_mqtt)
        return -1;

    return 0;
}

static void
log_via_mqtt_thread_free(void)
{
    if (app_config.log_via_mqtt) {
        if (mqtt_context.mosq) {
            mosquitto_disconnect(mqtt_context.mosq);
            mosquitto_loop_stop(mqtt_context.mosq, true);
            mosquitto_destroy(mqtt_context.mosq);
        }
        mosquitto_lib_cleanup();
    }
}

static int
log_mqtt_init(void *data)
{
    struct thread_context *thread_context = (struct thread_context *)data;

    if (app_config.log_via_mqtt == false)
        return 0;
    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.log_via_mqtt_thread_cpu;

    return log_via_mqtt_init();
}

static int
log_mqtt_launch(void *data)
{
    struct thread_context *thread_context = data;

    if (app_config.log_via_mqtt == false)
        return 0;
    if (log_via_mqtt_thread_create(thread_context))
        return -1;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "LogMQTT lcore %u is already running\n", thread_context->lcore_id);
    return rte_eal_remote_launch(log_via_mqtt_thread_routine, thread_context,
                                 thread_context->lcore_id);
}

static void
log_mqtt_deinit(void *data __rte_unused)
{

    if (app_config.log_via_mqtt == false)
        return;
    if (mqtt_context.mqtt_pool)
        rte_mempool_free(mqtt_context.mqtt_pool);
    if (mqtt_context.mqtt_ring)
        rte_ring_free(mqtt_context.mqtt_ring);
    log_via_mqtt_thread_free();
}

FUNCTION_REGISTER(log_mqtt, LOG_VIA_MQTT_IDX);

#endif

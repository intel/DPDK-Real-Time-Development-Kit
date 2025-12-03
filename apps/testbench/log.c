// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2020-2023 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rte_common.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_os.h>
#include <rte_per_lcore.h>
#include <rte_ring.h>
#include <rte_thread.h>

#include "config.h"
#include "functions.h"
#include "log.h"
#include "stat.h"
#include "thread.h"
#include "utils.h"

#define LOG_BURST_COUNT 128

typedef struct log_private {
    FILE *file_handle;
    struct rte_ring *global_ring_buffer;        // global ring buffer
    struct rte_mempool *log_pool;               // memory pool for log messages
    int socket_id;                              // socket ID
    enum log_level current_level;               // current log level
    FILE *file_tracing_on;                      // file handle for tracing on error
    uint64_t messages_dropped;                  // number of dropped messages
} log_private_t;

static log_private_t log_private = {0};

static int log_thread_create(struct thread_context *thread_context);

static int
__log_init(struct thread_context *thread_context)
{
    thread_context->private_data = &log_private;
    int socket_id                = rte_socket_id();

    log_private.global_ring_buffer =
        rte_ring_create("log", LOG_RING_SIZE, socket_id, RING_F_SC_DEQ | RING_F_MP_RTS_ENQ);
    if (!log_private.global_ring_buffer)
        return -ENOMEM;

    log_private.log_pool =
        rte_mempool_create("log_pool", LOG_MEMPOOL_BUFFER_COUNT, LOG_MEMPOOL_BUFFER_SIZE, 64, 0,
                           NULL, NULL, NULL, NULL, socket_id, 0);
    if (!log_private.log_pool)
        return -ENOMEM;

    /* Default */
    log_private.current_level = LOG_LEVEL_DEBUG;

    if (!strcasecmp(app_config.log_level, "Debug"))
        log_private.current_level = LOG_LEVEL_DEBUG;
    if (!strcasecmp(app_config.log_level, "Info"))
        log_private.current_level = LOG_LEVEL_INFO;
    if (!strcasecmp(app_config.log_level, "Warning"))
        log_private.current_level = LOG_LEVEL_WARNING;
    if (!strcasecmp(app_config.log_level, "Error"))
        log_private.current_level = LOG_LEVEL_ERROR;

    if (app_config.debug_stop_trace_on_error) {
        log_private.file_tracing_on = fopen("/sys/kernel/debug/tracing/tracing_on", "w");
        if (!log_private.file_tracing_on)
            return -errno;
    }

    return 0;
}

static const char *
log_level_to_string(enum log_level level)
{
    if (level == LOG_LEVEL_DEBUG)
        return "DEBUG";
    if (level == LOG_LEVEL_INFO)
        return "INFO";
    if (level == LOG_LEVEL_WARNING)
        return "WARNING";
    if (level == LOG_LEVEL_ERROR)
        return "ERROR";

    return NULL;
}

void
log_message(enum log_level level, const char *format, ...)
{
    unsigned char *buffer;
    int written, len, ret;
    struct timespec time;
    va_list args;
    char *p;

    /* Stop trace on error if desired. */
    if (level == LOG_LEVEL_ERROR && app_config.debug_stop_trace_on_error)
        fprintf(log_private.file_tracing_on, "0\n");

    /* Log message only if log level fulfilled. */
    if (level > log_private.current_level)
        return;

    if (rte_mempool_get(log_private.log_pool, (void **)&buffer) < 0) {
        log_private.messages_dropped++;
        return;
    }
    /* Log each message with time stamps. */
    ret = clock_gettime(app_config.application_clock_id, &time);
    if (ret)
        memset(&time, '\0', sizeof(time));

    len = LOG_MEMPOOL_BUFFER_SIZE - sizeof(uint16_t);
    p   = (char *)(buffer + sizeof(uint16_t));

    written = snprintf(p, len, "[%8ld.%9ld]: [%s]: ", time.tv_sec, time.tv_nsec,
                       log_level_to_string(level));
    p += written;
    len -= written;

    va_start(args, format);
    written += vsnprintf(p, len, format, args);
    va_end(args);

    *(uint16_t *)buffer = (uint16_t)written;
    rte_ring_enqueue(log_private.global_ring_buffer, buffer);
}

static void
log_add_traffic_class(const char *name, enum stat_frame_type frame_type, char **buffer,
                      size_t *length)
{
    const struct statistics *stat = stat_get_global_statistics(frame_type);
    int written;

    written =
        snprintf(*buffer, *length,
                 "%sSent=%" PRIu64 " | %sReceived=%" PRIu64 " | %sRttMin=%" PRIu64
                 " [us] | %sRttMax=%" PRIu64 " [us] | %sRttAvg=%lf [us] | %sOnewayMin=%" PRIu64
                 " [us] | %sOnewayMax=%" PRIu64 " [us] | %sOnewayAvg=%lf [us] | ",
                 name, stat->frames_sent, name, stat->frames_received, name, stat->round_trip_min,
                 name, stat->round_trip_max, name, stat->round_trip_avg, name, stat->oneway_min,
                 name, stat->oneway_max, name, stat->oneway_avg);
    *buffer += written;
    *length -= written;

    if (stat_frame_type_is_real_time(frame_type)) {
        written = snprintf(*buffer, *length,
                           "%sRttOutliers=%" PRIu64 " | %sOnewayOutliers=%" PRIu64 " | ", name,
                           stat->round_trip_outliers, name, stat->oneway_outliers);
        *buffer += written;
        *length -= written;
    }

    written = snprintf(*buffer, *length,
                       "%sOutOfOrderErrors=%" PRIu64 " | %sFrameIdErrors=%" PRIu64
                       " | %sPayloadErrors=%" PRIu64 " | ",
                       name, stat->out_of_order_errors, name, stat->frame_id_errors, name,
                       stat->payload_errors);
    *buffer += written;
    *length -= written;

    written =
        snprintf(*buffer, *length, "%sRxNextTimeAvg=%0.2lf [us] | %sTxNextTimeAvg=%0.2lf [us] | ",
                 name, stat->rx_nexttime_avg, name, stat->tx_nexttime_avg);
    *buffer += written;
    *length -= written;

    written = snprintf(
        *buffer, *length,
        "%sRxTimerAvg=%0.2lf [ns] | %sTxTimerAvg=%0.2lf [ns] | %sTxGenTimerAvg=%0.2lf [ns] | ",
        name, stat->rx_timer_avg, name, stat->tx_timer_avg, name, stat->txgen_timer_avg);
    *buffer += written;
    *length -= written;
}

static void
log_add_logging_stats(char **buffer, size_t *length)
{
    size_t written;

    written = snprintf(*buffer, *length, "LogDrops=%" PRIu64 " | ", log_private.messages_dropped);

    *buffer += written;
    *length -= written;
}

static int
log_thread_routine(void *data)
{
    struct thread_context *thread_context = data;
    uint64_t period                       = app_config.log_thread_period_ns;
    struct timespec time;
    int ret;

    if (log_thread_create(thread_context))
        return -1;

    /*
     * Write the content of the LogBuffer periodically to disk.  This thread can run with low
     * priority to not influence to Application Tasks that much.
     */
    ret = clock_gettime(app_config.application_clock_id, &time);
    if (ret) {
        fprintf(stderr, "Log: clock_gettime() failed: %s!\n", strerror(errno));
        return -1;
    }

    while (!thread_context->stop) {
        char *buffer[LOG_BURST_COUNT];
        size_t stat_message_length;
        char stat_message[8192] = {0}, *p;

        /* Wait until next period */
        increment_period(&time, period);
        do {
            ret = clock_nanosleep(app_config.application_clock_id, TIMER_ABSTIME, &time, NULL);
        } while (ret == EINTR);

        /* Log statistics once per logging period. */
        p                   = stat_message;
        *p                  = '\0';
        stat_message_length = sizeof(stat_message) - 1;

        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_high))
            log_add_traffic_class("TsnHigh", TSN_HIGH_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_low))
            log_add_traffic_class("TsnLow", TSN_LOW_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rtc))
            log_add_traffic_class("Rtc", RTC_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rta))
            log_add_traffic_class("Rta", RTA_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(dcp))
            log_add_traffic_class("Dcp", DCP_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(lldp))
            log_add_traffic_class("Lldp", LLDP_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_high))
            log_add_traffic_class("UdpHigh", UDP_HIGH_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_low))
            log_add_traffic_class("UdpLow", UDP_LOW_FRAME_TYPE, &p, &stat_message_length);
        if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(l2))
            log_add_traffic_class("L2", L2_FRAME_TYPE, &p, &stat_message_length);

        log_add_logging_stats(&p, &stat_message_length);
        log_message(LOG_LEVEL_INFO, "%s\n", stat_message);

        for (;;) {
            int n;

            /* Fetch data */
            if ((n = rte_ring_dequeue_burst(log_private.global_ring_buffer, (void **)buffer,
                                            LOG_BURST_COUNT, NULL)) == 0)
                break;

            for (int i = 0; i < n; i++) {
                /* Write down to disk */
                fwrite(buffer[i] + sizeof(uint16_t), sizeof(char), *(uint16_t *)buffer[i],
                       log_private.file_handle);
            }
            rte_mempool_put_bulk(log_private.log_pool, (void *const *)buffer, n);
        }
        fflush(log_private.file_handle);
    }

    return 0;
}

static int
log_thread_create(struct thread_context *thread_context __rte_unused)
{
    log_private.file_handle = fopen(app_config.log_file, "w+");
    if (!log_private.file_handle)
        return -1;
    return 0;
}

static void
log_thread_free(struct thread_context *thread_context)
{
    if (!thread_context)
        return;

    if (log_private.log_pool)
        rte_mempool_free(log_private.log_pool);
    if (log_private.global_ring_buffer)
        rte_ring_free(log_private.global_ring_buffer);

    fclose(log_private.file_handle);

    if (app_config.debug_stop_trace_on_error)
        fclose(log_private.file_tracing_on);
}

static int
log_init(void *data)
{
    struct thread_context *thread_context = (struct thread_context *)data;

    // Set the lcore ID for the thread to use from the configuration
    thread_context->lcore_id = app_config.log_thread_cpu;

    return __log_init(thread_context);
}

static int
log_launch(void *data)
{
    struct thread_context *thread_context = data;

    if (rte_eal_get_lcore_state(thread_context->lcore_id) == RUNNING)
        rte_exit(EXIT_FAILURE, "Log lcore %u is already running\n", thread_context->lcore_id);

    return rte_eal_remote_launch(log_thread_routine, thread_context, thread_context->lcore_id);
}

static void
log_deinit(void *data)
{
    struct thread_context *thread_context = data;

    log_thread_free(thread_context);
}

FUNCTION_REGISTER(log, LOG_IDX);

// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2021-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <errno.h>
#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rte_common.h>

#include "config.h"
#include "functions.h"
#include "hist.h"
#include "log.h"
#include "logviamqtt.h"
#include "lport.h"
#include "stat.h"
#include "utils.h"        // tools

typedef struct stat_private {
    struct statistics global_statistics[NUM_FRAME_TYPES];
    struct statistics statistics_per_period[NUM_FRAME_TYPES];
    struct round_trip_context round_trip_contexts[NUM_FRAME_TYPES];
    uint64_t rtt_expected_rt_limit;
    FILE *file_tracing_on;
    FILE *file_trace_marker;
} stat_private_t;

static stat_private_t stat_private = {0};

const char *stat_frame_type_names[NUM_FRAME_TYPES] = {"TsnHigh", "TsnLow",  "Rtc",    "Rta", "Dcp",
                                                      "Lldp",    "UdpHigh", "UdpLow", "L2"};

/*
 * Keep 1024 periods of backlog available. If a frame is received later than 1024 periods after
 * sending, it's a bug in any case.
 *
 * E.g. A period of 500us results in a backlog of 500ms.
 */
#define STAT_MAX_BACKLOG 1024

struct statistics *
stat_get_global_statistics(enum stat_frame_type frame_type)
{
    return &stat_private.global_statistics[frame_type];
}

static int
__stat_init(void *data)
{
    struct thread_context *thread_context = data;

    thread_context->private_data = &stat_private;

    if (!is_mirror_mode()) {
        bool allocation_error = false;

        stat_private.round_trip_contexts[TSN_HIGH_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.tsn_high_num_frames_per_cycle;
        stat_private.round_trip_contexts[TSN_LOW_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.tsn_low_num_frames_per_cycle;
        stat_private.round_trip_contexts[RTC_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.rtc_num_frames_per_cycle;
        stat_private.round_trip_contexts[RTA_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.rta_num_frames_per_cycle;
        stat_private.round_trip_contexts[DCP_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.dcp_num_frames_per_cycle;
        stat_private.round_trip_contexts[LLDP_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.lldp_num_frames_per_cycle;
        stat_private.round_trip_contexts[UDP_HIGH_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.udp_high_num_frames_per_cycle;
        stat_private.round_trip_contexts[UDP_LOW_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.udp_low_num_frames_per_cycle;
        stat_private.round_trip_contexts[L2_FRAME_TYPE].backlog_len =
            STAT_MAX_BACKLOG * app_config.l2_num_frames_per_cycle;

        for (int i = 0; i < NUM_FRAME_TYPES; i++) {
            struct round_trip_context *current_context = &stat_private.round_trip_contexts[i];

            current_context->backlog =
                rte_calloc_socket("backlog", current_context->backlog_len, sizeof(backlog_t),
                                  RTE_CACHE_LINE_SIZE, rte_socket_id());
            allocation_error |= !current_context->backlog;
        }

        if (allocation_error)
            return -ENOMEM;
    }

    for (int i = 0; i < NUM_FRAME_TYPES; i++) {
        struct statistics *current_stats = stat_get_global_statistics(i);

        current_stats->round_trip_min = UINT64_MAX;
        current_stats->round_trip_max = 0;
        current_stats->oneway_min     = UINT64_MAX;
        current_stats->oneway_max     = 0;

        current_stats                 = &stat_private.statistics_per_period[i];
        current_stats->round_trip_min = UINT64_MAX;
        current_stats->round_trip_max = 0;
        current_stats->oneway_min     = UINT64_MAX;
        current_stats->oneway_max     = 0;
    }

    if (app_config.debug_stop_trace_on_outlier) {
        stat_private.file_tracing_on = fopen("/sys/kernel/debug/tracing/tracing_on", "w");
        if (!stat_private.file_tracing_on)
            return -errno;
        stat_private.file_trace_marker = fopen("/sys/kernel/debug/tracing/trace_marker", "w");
        if (!stat_private.file_trace_marker) {
            fclose(stat_private.file_tracing_on);
            return -errno;
        }
    }

    /*
     * The expected round trip limit for RT traffic classes is below < 2 * cycle time.
     * Stored in us.
     */
    stat_private.rtt_expected_rt_limit = app_config.application_base_cycle_time_ns * 2;
    stat_private.rtt_expected_rt_limit /= 1000;

    return 0;
}

static void
__stat_free(void)
{
    for (int i = 0; i < NUM_FRAME_TYPES; i++)
        rte_free(stat_private.round_trip_contexts[i].backlog);

    if (app_config.debug_stop_trace_on_outlier) {
        fclose(stat_private.file_tracing_on);
        fclose(stat_private.file_trace_marker);
    }
}

static inline void
stat_update_min_max(uint64_t new_value, uint64_t *min, uint64_t *max)
{
    *max = (new_value > *max) ? new_value : *max;
    *min = (new_value < *min) ? new_value : *min;
}

static void
stats_reset_stats(struct statistics *stats)
{
    memset(stats, 0, sizeof(struct statistics));

    stats->round_trip_min = USEC_PER_SEC / 4;
    stats->oneway_min     = USEC_PER_SEC / 4;

    for (int i = 0; i < NUM_FRAME_TYPES; i++) {
        struct statistics *per_period = &stat_private.statistics_per_period[i];

        memset(per_period, 0, sizeof(struct statistics));
        per_period->round_trip_min = USEC_PER_SEC / 4;
        per_period->oneway_min     = USEC_PER_SEC / 4;
    }
}

void
stats_reset_all_stats(void)
{
    for (int i = 0; i < NUM_FRAME_TYPES; i++)
        stats_reset_stats(stat_get_global_statistics(i));
}

#if defined(WITH_MQTT)
static void
stat_frame_received_per_period(enum stat_frame_type frame_type, uint64_t curr_time,
                               uint64_t rt_time, uint64_t oneway_time, bool out_of_order,
                               bool payload_mismatch, bool frame_id_mismatch)
{
    struct statistics *stat_per_period = &stat_private.statistics_per_period[frame_type];
    uint64_t elapsed_t;

    if (stat_per_period->first_time_stamp == 0)
        stat_per_period->first_time_stamp = curr_time;

    /*
     * Test if the amount of time specified in the config is arrived.  if true this will be the
     * last point to be taken into stats per period
     */
    elapsed_t = curr_time - stat_per_period->first_time_stamp;
    if (elapsed_t >= app_config.stats_collection_interval_ns) {
        stat_per_period->ready           = true;
        stat_per_period->last_time_stamp = curr_time;
    }

    if (!is_mirror_mode()) {
        if (stat_frame_type_is_real_time(frame_type) &&
            rt_time > stat_private.rtt_expected_rt_limit)
            stat_per_period->round_trip_outliers++;
        stat_update_min_max(rt_time, &stat_per_period->round_trip_min,
                            &stat_per_period->round_trip_max);

        stat_per_period->round_trip_count++;
        stat_per_period->round_trip_sum += rt_time;
        stat_per_period->round_trip_avg =
            stat_per_period->round_trip_sum / (double)stat_per_period->round_trip_count;
    }

    stat_update_min_max(oneway_time, &stat_per_period->oneway_min, &stat_per_period->oneway_max);

    if (stat_frame_type_is_real_time(frame_type) &&
        oneway_time > stat_private.rtt_expected_rt_limit / 2)
        stat_per_period->oneway_outliers++;

    stat_per_period->oneway_count++;
    stat_per_period->oneway_sum += oneway_time;
    stat_per_period->oneway_avg =
        stat_per_period->oneway_sum / (double)stat_per_period->oneway_count;

    stat_per_period->frames_received++;
    stat_per_period->out_of_order_errors += out_of_order;
    stat_per_period->payload_errors += payload_mismatch;
    stat_per_period->frame_id_errors += frame_id_mismatch;

    /*
     * Final bits can be used in the logger reseting copying actual values and reseting the
     * preparation
     */
    if (stat_per_period->ready) {
        log_via_mqtt_stats(frame_type, &stat_private.statistics_per_period[frame_type]);
        stats_reset_stats(&stat_private.statistics_per_period[frame_type]);
    }
}

static void
stat_frame_sent_per_period(enum stat_frame_type frame_type)
{
    struct statistics *stat_per_period = &stat_private.statistics_per_period[frame_type];

    /* Just increment the Tx counter. The reset per period is done by the Rx part. */
    stat_per_period->frames_sent++;
}
#else
static void
stat_frame_received_per_period(enum stat_frame_type frame_type, uint64_t curr_time,
                               uint64_t rt_time, bool out_of_order, bool payload_mismatch,
                               bool frame_id_mismatch, uint64_t tx_timestamp)
{
    (void)frame_type;
    (void)curr_time;
    (void)rt_time;
    (void)out_of_order;
    (void)payload_mismatch;
    (void)frame_id_mismatch;
    (void)tx_timestamp;
}

static void
stat_frame_sent_per_period(enum stat_frame_type frame_type)
{
    (void)frame_type;
}
#endif

void
stat_frame_sent(enum stat_frame_type frame_type, uint64_t cycle_number, uint64_t timestamp)
{
    struct round_trip_context *rtt = &stat_private.round_trip_contexts[frame_type];
    struct statistics *stat        = stat_get_global_statistics(frame_type);

    log_message(LOG_LEVEL_DEBUG, "%s: frame[%" PRIu64 "] sent\n",
                stat_frame_type_to_string(frame_type), cycle_number);

    if (!is_mirror_mode()) {
        backlog_t *b = &rtt->backlog[cycle_number % rtt->backlog_len];

        /* Record Tx timestamp and cycle_number in backlog */
        b->timestamp    = timestamp;
        b->cycle_number = cycle_number;
    }

    /* Increment stats */
    stat_frame_sent_per_period(frame_type);
    stat->frames_sent++;
}

void
stat_next_time(enum stat_frame_type frame_type, uint64_t rx_nexttime, uint64_t tx_nexttime)
{
    struct statistics *stat = stat_get_global_statistics(frame_type);

    if (rx_nexttime) {
        stat->rx_nexttime_cnt++;
        stat->rx_nexttime_sum += (rx_nexttime / 1000UL);        // convert to micro-seconds
        stat->rx_nexttime_avg = stat->rx_nexttime_sum / (double)stat->rx_nexttime_cnt;
    }
    if (tx_nexttime) {
        stat->tx_nexttime_cnt++;
        stat->tx_nexttime_sum += (tx_nexttime / 1000UL);        // convert to micro-seconds
        stat->tx_nexttime_avg = stat->tx_nexttime_sum / (double)stat->tx_nexttime_cnt;
    }
}

void
stat_timer_add(enum stat_frame_type frame_type, struct stat_avg *avg)
{
    struct statistics *stat = stat_get_global_statistics(frame_type);

    log_message(LOG_LEVEL_DEBUG, "%s: Rx:  %'10.2lf, Tx: %10.2lf ns %s\n",
                stat_frame_type_to_string(frame_type), avg->rx, avg->tx,
                (avg->rx > 1000.0 || avg->tx > 1000.0) ? ">>>" : "");
    stat->rx_timer_avg    = avg->rx;
    stat->tx_timer_avg    = avg->tx;
    stat->txgen_timer_avg = avg->txgen;
}

void
stat_frame_received(enum stat_frame_type frame_type, uint64_t cycle_number, bool out_of_order,
                    bool payload_mismatch, bool frame_id_mismatch, uint64_t tx_timestamp)
{
    struct round_trip_context *rtt = &stat_private.round_trip_contexts[frame_type];
    const bool histogram           = app_config.stats_histogram_enabled;
    struct statistics *stat        = stat_get_global_statistics(frame_type);
    uint64_t rt_time               = 0, curr_time, oneway_time;
    bool outlier                   = false;

    log_message(LOG_LEVEL_DEBUG, "%s: frame[%" PRIu64 "] received\n",
                stat_frame_type_to_string(frame_type), cycle_number);

    /* Record Rx timestamp in nano-seconds */
    curr_time = clock_gettime_ns();

    if (!is_mirror_mode()) {
        backlog_t *b = &rtt->backlog[cycle_number % rtt->backlog_len];

        rt_time = curr_time - b->timestamp;
        rt_time /= 1000;

        if (b->cycle_number != cycle_number)
            log_message(LOG_LEVEL_DEBUG, "%s: frame[%'" PRIu64 "] != cycle_number %'" PRIu64 "\n",
                        stat_frame_type_to_string(frame_type), cycle_number, b->cycle_number);
        b->cycle_number = 0;
        b->timestamp    = 0;

        stat_update_min_max(rt_time, &stat->round_trip_min, &stat->round_trip_max);

        if (stat_frame_type_is_real_time(frame_type) &&
            rt_time > stat_private.rtt_expected_rt_limit) {
            stat->round_trip_outliers++;
            outlier = true;
        }
        stat->round_trip_count++;
        stat->round_trip_sum += rt_time;
        stat->round_trip_avg = stat->round_trip_sum / (double)stat->round_trip_count;

        /* Update histogram */
        if (histogram)
            histogram_update(frame_type, rt_time);
    }

    if (curr_time < tx_timestamp) {
        log_message(LOG_LEVEL_WARNING,
                    "%-8s: frame[%'" PRIu64 "] curr:%'" PRIu64 " timestamp:%'18" PRIu64 "\n",
                    stat_frame_type_to_string(frame_type), cycle_number, curr_time, tx_timestamp);
		curr_time = tx_timestamp;
    }
    oneway_time = curr_time - tx_timestamp;
    oneway_time /= 1000;

    stat_update_min_max(oneway_time, &stat->oneway_min, &stat->oneway_max);

    if (stat_frame_type_is_real_time(frame_type) &&
        oneway_time > stat_private.rtt_expected_rt_limit / 2) {
        log_message(LOG_LEVEL_DEBUG, "%s: frame[%'" PRIu64 "] oneway outlier %'" PRIu64 "us\n",
                    stat_frame_type_to_string(frame_type), cycle_number, oneway_time);
        stat->oneway_outliers++;
        outlier = true;
    }
    stat->oneway_count++;
    stat->oneway_sum += oneway_time;
    stat->oneway_avg = stat->oneway_sum / (double)stat->oneway_count;

    stat_frame_received_per_period(frame_type, curr_time, rt_time, oneway_time, out_of_order,
                                   payload_mismatch, frame_id_mismatch);

    /* Stop tracing after certain amount of time */
    if (app_config.debug_stop_trace_on_outlier && outlier) {
        fprintf(stat_private.file_trace_marker,
                "Outlier hit: %" PRIu64 " [us] -- Type: %s -- Cycle Counter: %" PRIu64 "\n",
                rt_time ? rt_time : oneway_time, stat_frame_type_to_string(frame_type),
                cycle_number);
        fprintf(stat_private.file_tracing_on, "0\n");
        fprintf(stat_private.file_tracing_on,
                "Outlier hit: %" PRIu64 " [us] -- Type: %s -- Cycle Counter: %" PRIu64 "\n",
                rt_time ? rt_time : oneway_time, stat_frame_type_to_string(frame_type),
                cycle_number);
        fclose(stat_private.file_trace_marker);
        fclose(stat_private.file_tracing_on);
        exit(EXIT_SUCCESS);
    }

    /* Increment stats */
    stat->frames_received++;
    stat->out_of_order_errors += out_of_order;
    stat->payload_errors += payload_mismatch;
    stat->frame_id_errors += frame_id_mismatch;
}

static int
stat_init(void *data __rte_unused)
{
    return __stat_init(data);
}

static int
stat_launch(void *data __rte_unused)
{
    return 0;
}

static void
stat_deinit(void *data __rte_unused)
{
    __stat_free();
}

FUNCTION_REGISTER(stat, STAT_IDX);

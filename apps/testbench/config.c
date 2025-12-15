// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include <linux/if_ether.h>

#include <rte_common.h>

#include "config.h"
#include "net_def.h"
#include "security.h"
#include "utils.h"

#include "dcp_thread.h"
#include "functions.h"
#include "layer2_thread.h"
#include "lldp_thread.h"
#include "lport.h"
#include "rta_thread.h"
#include "rtc_thread.h"
#include "tsn_thread.h"
#include "udp_thread.h"

#define DEFAULT_MBUF_COUNT  (16 * 1024)
#define NUM_RX_DESC_DEFAULT (4 * 1024)
#define NUM_TX_DESC_DEFAULT (4 * 1024)

struct application_config app_config = {0};

static void __config_free(void);

static const char *config_file_path;

void
config_set_file(const char *config_file)
{
    config_file_path = config_file;
}

#define max_queue(enabled, id)                        \
    if (app_config.enabled) {                         \
        int pid           = lport2pid(app_config.id); \
        int qid           = lport2qid(app_config.id); \
        qid_t *q          = &app_config.qinfo[pid];   \
        q->qids[q->cnt++] = qid;                      \
    }

/* The configuration file is YAML based. Use libyaml to parse it. */
int
config_read_from_file(const char *config_file)
{
    bool base_time_seen = false;
    int ret, state_key = 0;
    yaml_parser_t parser;
    yaml_token_t token;
    const char *value;
    char *key = NULL;
    FILE *f;

    if (!config_file)
        return -EINVAL;

    f = fopen(config_file, "r");
    if (!f) {
        perror("fopen() failed");
        return -EIO;
    }

    ret = yaml_parser_initialize(&parser);
    if (!ret) {
        ret = -EINVAL;
        fprintf(stderr, "Failed to initialize YAML parser\n");
        goto err_yaml;
    }

    yaml_parser_set_input_file(&parser, f);

    do {
        char *endptr;

        ret = yaml_parser_scan(&parser, &token);
        if (!ret) {
            ret = -EINVAL;
            fprintf(stderr, "%s: Failed to parse YAML file!\n", __func__);
            goto err_parse;
        }

        switch (token.type) {
        case YAML_KEY_TOKEN:
            state_key = 1;
            break;
        case YAML_VALUE_TOKEN:
            state_key = 0;
            break;
        case YAML_SCALAR_TOKEN:
            value = (const char *)token.data.scalar.value;
            if (state_key) {
                free(key);
                key = NULL;
                /* Save key */
                key = strdup(value);
                if (!key) {
                    fprintf(stderr, "%s: No memory left!\n", __func__);
                    goto err_parse;
                }

                continue;
            }

            if (!key)
                continue;

            errno = 0;        // Cleanup errno for future use
            /* Switch value */
            CONFIG_STORE_CLOCKID_PARAM(ApplicationClockId, application_clock_id);
            CONFIG_STORE_ULONG_PARAM(ApplicationBaseCycleTimeNS, application_base_cycle_time_ns);
            CONFIG_STORE_ULONG_PARAM(ApplicationTxBaseOffsetNS, application_tx_base_offset_ns);
            CONFIG_STORE_ULONG_PARAM(ApplicationRxBaseOffsetNS, application_rx_base_offset_ns);
            CONFIG_STORE_UINT_PARAM(ApplicationNumMbufs, application_num_mbufs);
            CONFIG_STORE_UINT_PARAM(ApplicationMbufSize, application_mbuf_size);
            CONFIG_STORE_UINT_PARAM(ApplicationCacheSize, application_cache_size);
            CONFIG_STORE_UINT_PARAM(ApplicationNumRxDescriptors, application_num_rx_descriptors);
            CONFIG_STORE_UINT_PARAM(ApplicationNumTxDescriptors, application_num_tx_descriptors);
            CONFIG_STORE_BOOL_PARAM(ApplicationLinkHalfDuplex, application_link_half_duplex);
            CONFIG_STORE_UINT_PARAM(ApplicationLinkSpeed, application_link_speed);
            CONFIG_STORE_UINT_PARAM(ApplicationWaitTimeBeforeStart,
                                    application_wait_time_before_start);

            CONFIG_STORE_BOOL_PARAM(PktHandlerEnabled, pkt_handler_enabled);
            CONFIG_STORE_INT_PARAM(PktHandlerThreadCpu, pkt_handler_thread_cpu);

            CONFIG_STORE_BOOL_PARAM(TsnHighEnabled, tsn_high_enabled);
            CONFIG_STORE_LPORT_PARAM(TsnHighLPortID, tsn_high_lport_id);
            CONFIG_STORE_BOOL_PARAM(TsnHighTxTimeEnabled, tsn_high_tx_time_enabled);
            CONFIG_STORE_BOOL_PARAM(TsnHighIgnoreRxErrors, tsn_high_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(TsnHighTxTimeOffsetNS, tsn_high_tx_time_offset_ns);
            CONFIG_STORE_INT_PARAM(TsnHighVid, tsn_high_vid);
            CONFIG_STORE_INT_PARAM(TsnHighPcp, tsn_high_pcp);
            CONFIG_STORE_ULONG_PARAM(TsnHighNumFramesPerCycle, tsn_high_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(TsnHighPayloadPattern, tsn_high_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(TsnHighFrameLength, tsn_high_frame_length);
            CONFIG_STORE_SECURITY_MODE_PARAM(TsnHighSecurityMode, tsn_high_security_mode);
            CONFIG_STORE_SECURITY_ALGORITHM_PARAM(TsnHighSecurityAlgorithm,
                                                  tsn_high_security_algorithm);
            CONFIG_STORE_STRING_PARAM(TsnHighSecurityKey, tsn_high_security_key);
            CONFIG_STORE_STRING_PARAM(TsnHighSecurityIvPrefix, tsn_high_security_iv_prefix);
            CONFIG_STORE_INT_PARAM(TsnHighThreadCpu, tsn_high_thread_cpu);
            CONFIG_STORE_MAC_PARAM(TsnHighDestination, tsn_high_destination);

            CONFIG_STORE_BOOL_PARAM(TsnLowEnabled, tsn_low_enabled);
            CONFIG_STORE_BOOL_PARAM(TsnLowTxTimeEnabled, tsn_low_tx_time_enabled);
            CONFIG_STORE_BOOL_PARAM(TsnLowIgnoreRxErrors, tsn_low_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(TsnLowTxTimeOffsetNS, tsn_low_tx_time_offset_ns);
            CONFIG_STORE_INT_PARAM(TsnLowVid, tsn_low_vid);
            CONFIG_STORE_INT_PARAM(TsnLowPcp, tsn_low_pcp);
            CONFIG_STORE_ULONG_PARAM(TsnLowNumFramesPerCycle, tsn_low_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(TsnLowPayloadPattern, tsn_low_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(TsnLowFrameLength, tsn_low_frame_length);
            CONFIG_STORE_SECURITY_MODE_PARAM(TsnLowSecurityMode, tsn_low_security_mode);
            CONFIG_STORE_SECURITY_ALGORITHM_PARAM(TsnLowSecurityAlgorithm,
                                                  tsn_low_security_algorithm);
            CONFIG_STORE_STRING_PARAM(TsnLowSecurityKey, tsn_low_security_key);
            CONFIG_STORE_STRING_PARAM(TsnLowSecurityIvPrefix, tsn_low_security_iv_prefix);
            CONFIG_STORE_LPORT_PARAM(TsnLowLPortID, tsn_low_lport_id);
            CONFIG_STORE_INT_PARAM(TsnLowThreadCpu, tsn_low_thread_cpu);
            CONFIG_STORE_MAC_PARAM(TsnLowDestination, tsn_low_destination);

            CONFIG_STORE_BOOL_PARAM(RtcEnabled, rtc_enabled);
            CONFIG_STORE_BOOL_PARAM(RtcIgnoreRxErrors, rtc_ignore_rx_errors);
            CONFIG_STORE_INT_PARAM(RtcVid, rtc_vid);
            CONFIG_STORE_INT_PARAM(RtcPcp, rtc_pcp);
            CONFIG_STORE_ULONG_PARAM(RtcNumFramesPerCycle, rtc_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(RtcPayloadPattern, rtc_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(RtcFrameLength, rtc_frame_length);
            CONFIG_STORE_SECURITY_MODE_PARAM(RtcSecurityMode, rtc_security_mode);
            CONFIG_STORE_SECURITY_ALGORITHM_PARAM(RtcSecurityAlgorithm, rtc_security_algorithm);
            CONFIG_STORE_STRING_PARAM(RtcSecurityKey, rtc_security_key);
            CONFIG_STORE_STRING_PARAM(RtcSecurityIvPrefix, rtc_security_iv_prefix);
            CONFIG_STORE_LPORT_PARAM(RtcLPortID, rtc_lport_id);
            CONFIG_STORE_INT_PARAM(RtcThreadCpu, rtc_thread_cpu);
            CONFIG_STORE_MAC_PARAM(RtcDestination, rtc_destination);

            CONFIG_STORE_BOOL_PARAM(RtaEnabled, rta_enabled);
            CONFIG_STORE_BOOL_PARAM(RtaIgnoreRxErrors, rta_ignore_rx_errors);
            CONFIG_STORE_INT_PARAM(RtaVid, rta_vid);
            CONFIG_STORE_INT_PARAM(RtaPcp, rta_pcp);
            CONFIG_STORE_ULONG_PARAM(RtaBurstPeriodNS, rta_burst_period_ns);
            CONFIG_STORE_ULONG_PARAM(RtaNumFramesPerCycle, rta_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(RtaPayloadPattern, rta_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(RtaFrameLength, rta_frame_length);
            CONFIG_STORE_SECURITY_MODE_PARAM(RtaSecurityMode, rta_security_mode);
            CONFIG_STORE_SECURITY_ALGORITHM_PARAM(RtaSecurityAlgorithm, rta_security_algorithm);
            CONFIG_STORE_STRING_PARAM(RtaSecurityKey, rta_security_key);
            CONFIG_STORE_STRING_PARAM(RtaSecurityIvPrefix, rta_security_iv_prefix);
            CONFIG_STORE_LPORT_PARAM(RtaLPortID, rta_lport_id);
            CONFIG_STORE_INT_PARAM(RtaThreadCpu, rta_thread_cpu);
            CONFIG_STORE_MAC_PARAM(RtaDestination, rta_destination);

            CONFIG_STORE_BOOL_PARAM(DcpEnabled, dcp_enabled);
            CONFIG_STORE_BOOL_PARAM(DcpIgnoreRxErrors, dcp_ignore_rx_errors);
            CONFIG_STORE_INT_PARAM(DcpVid, dcp_vid);
            CONFIG_STORE_INT_PARAM(DcpPcp, dcp_pcp);
            CONFIG_STORE_ULONG_PARAM(DcpBurstPeriodNS, dcp_burst_period_ns);
            CONFIG_STORE_ULONG_PARAM(DcpNumFramesPerCycle, dcp_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(DcpPayloadPattern, dcp_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(DcpFrameLength, dcp_frame_length);
            CONFIG_STORE_LPORT_PARAM(DcpLPortID, dcp_lport_id);
            CONFIG_STORE_INT_PARAM(DcpThreadCpu, dcp_thread_cpu);
            CONFIG_STORE_MAC_PARAM(DcpDestination, dcp_destination);

            CONFIG_STORE_BOOL_PARAM(LldpEnabled, lldp_enabled);
            CONFIG_STORE_BOOL_PARAM(LldpIgnoreRxErrors, lldp_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(LldpBurstPeriodNS, lldp_burst_period_ns);
            CONFIG_STORE_ULONG_PARAM(LldpNumFramesPerCycle, lldp_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(LldpPayloadPattern, lldp_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(LldpFrameLength, lldp_frame_length);
            CONFIG_STORE_LPORT_PARAM(LldpLPortID, lldp_lport_id);
            CONFIG_STORE_INT_PARAM(LldpThreadCpu, lldp_thread_cpu);
            CONFIG_STORE_MAC_PARAM(LldpDestination, lldp_destination);

            CONFIG_STORE_BOOL_PARAM(UdpHighEnabled, udp_high_enabled);
            CONFIG_STORE_BOOL_PARAM(UdpHighIgnoreRxErrors, udp_high_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(UdpHighBurstPeriodNS, udp_high_burst_period_ns);
            CONFIG_STORE_ULONG_PARAM(UdpHighNumFramesPerCycle, udp_high_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(UdpHighPayloadPattern, udp_high_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(UdpHighFrameLength, udp_high_frame_length);
            CONFIG_STORE_LPORT_PARAM(UdpHighLPortID, udp_high_lport_id);
            CONFIG_STORE_INT_PARAM(UdpHighThreadCpu, udp_high_thread_cpu);
            CONFIG_STORE_INT_PARAM(UdpHighPort, udp_high_port);
            CONFIG_STORE_INT_PARAM(UdpHighSrcPort, udp_high_src_port);
            CONFIG_STORE_STRING_PARAM(UdpHighDestination, udp_high_destination);
            CONFIG_STORE_STRING_PARAM(UdpHighSource, udp_high_source);
            CONFIG_STORE_MAC_PARAM(UdpHighMacDestination, udp_high_mac_destination);

            CONFIG_STORE_BOOL_PARAM(UdpLowEnabled, udp_low_enabled);
            CONFIG_STORE_BOOL_PARAM(UdpLowIgnoreRxErrors, udp_low_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(UdpLowBurstPeriodNS, udp_low_burst_period_ns);
            CONFIG_STORE_ULONG_PARAM(UdpLowNumFramesPerCycle, udp_low_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(UdpLowPayloadPattern, udp_low_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(UdpLowFrameLength, udp_low_frame_length);
            CONFIG_STORE_LPORT_PARAM(UdpLowLPortID, udp_low_lport_id);
            CONFIG_STORE_INT_PARAM(UdpLowThreadCpu, udp_low_thread_cpu);
            CONFIG_STORE_INT_PARAM(UdpLowPort, udp_low_port);
            CONFIG_STORE_INT_PARAM(UdpLowSrcPort, udp_low_src_port);
            CONFIG_STORE_STRING_PARAM(UdpLowDestination, udp_low_destination);
            CONFIG_STORE_STRING_PARAM(UdpLowSource, udp_low_source);
            CONFIG_STORE_MAC_PARAM(UdpLowMacDestination, udp_low_mac_destination);

            CONFIG_STORE_BOOL_PARAM(L2Enabled, l2_enabled);
            CONFIG_STORE_BOOL_PARAM(L2TxTimeEnabled, l2_tx_time_enabled);
            CONFIG_STORE_BOOL_PARAM(L2IgnoreRxErrors, l2_ignore_rx_errors);
            CONFIG_STORE_ULONG_PARAM(L2TxTimeOffsetNS, l2_tx_time_offset_ns);
            CONFIG_STORE_INT_PARAM(L2Vid, l2_vid);
            CONFIG_STORE_INT_PARAM(L2Pcp, l2_pcp);
            CONFIG_STORE_ETHER_TYPE(L2EtherType, l2_ether_type);
            CONFIG_STORE_ULONG_PARAM(L2NumFramesPerCycle, l2_num_frames_per_cycle);
            CONFIG_STORE_STRING_PARAM(L2PayloadPattern, l2_payload_pattern);
            CONFIG_STORE_ULONG_PARAM(L2FrameLength, l2_frame_length);
            CONFIG_STORE_LPORT_PARAM(L2LPortID, l2_lport_id);
            CONFIG_STORE_INT_PARAM(L2ThreadCpu, l2_thread_cpu);
            CONFIG_STORE_MAC_PARAM(L2Destination, l2_destination);

            CONFIG_STORE_ULONG_PARAM(LogThreadPeriodNS, log_thread_period_ns);
            CONFIG_STORE_INT_PARAM(LogThreadPriority, log_thread_priority);
            CONFIG_STORE_INT_PARAM(LogThreadCpu, log_thread_cpu);
            CONFIG_STORE_STRING_PARAM(LogFile, log_file);
            CONFIG_STORE_STRING_PARAM(LogLevel, log_level);

            CONFIG_STORE_BOOL_PARAM(DebugStopTraceOnOutlier, debug_stop_trace_on_outlier);
            CONFIG_STORE_BOOL_PARAM(DebugStopTraceOnError, debug_stop_trace_on_error);
            CONFIG_STORE_BOOL_PARAM(DebugMonitorMode, debug_monitor_mode);
            CONFIG_STORE_MAC_PARAM(DebugMonitorDestination, debug_monitor_destination);

            CONFIG_STORE_BOOL_PARAM(StatsHistogramEnabled, stats_histogram_enabled);
            CONFIG_STORE_ULONG_PARAM(StatsHistogramMinimumNS, stats_histogram_minimum_ns);
            CONFIG_STORE_ULONG_PARAM(StatsHistogramMaximumNS, stats_histogram_maximum_ns);
            CONFIG_STORE_STRING_PARAM(StatsHistogramFile, stats_histogram_file);
            CONFIG_STORE_ULONG_PARAM(StatsCollectionIntervalNS, stats_collection_interval_ns);

            CONFIG_STORE_BOOL_PARAM(LogViaMQTT, log_via_mqtt);
            CONFIG_STORE_INT_PARAM(LogViaMQTTThreadPriority, log_via_mqtt_thread_priority);
            CONFIG_STORE_INT_PARAM(LogViaMQTTThreadCpu, log_via_mqtt_thread_cpu);
            CONFIG_STORE_ULONG_PARAM(LogViaMQTTThreadPeriodNS, log_via_mqtt_thread_period_ns);
            CONFIG_STORE_STRING_PARAM(LogViaMQTTBrokerIP, log_via_mqtt_broker_ip);
            CONFIG_STORE_INT_PARAM(LogViaMQTTBrokerPort, log_via_mqtt_broker_port);
            CONFIG_STORE_INT_PARAM(LogViaMQTTKeepAliveSecs, log_via_mqtt_keep_alive_secs);
            CONFIG_STORE_STRING_PARAM(LogViaMQTTMeasurementName, log_via_mqtt_measurement_name);

            if (!strcmp(key, "ApplicationBaseStartTimeNS"))
                base_time_seen = true;

            free(key);
            key = NULL;

        default:
            break;
        }

        if (token.type != YAML_STREAM_END_TOKEN)
            yaml_token_delete(&token);

    } while (token.type != YAML_STREAM_END_TOKEN);

    max_queue(pkt_handler_enabled, pkt_handler_lport_id);
    max_queue(tsn_high_enabled, tsn_high_lport_id);
    max_queue(tsn_low_enabled, tsn_low_lport_id);
    max_queue(rtc_enabled, rtc_lport_id);
    max_queue(rta_enabled, rta_lport_id);
    max_queue(dcp_enabled, dcp_lport_id);
    max_queue(lldp_enabled, lldp_lport_id);
    max_queue(udp_high_enabled, udp_high_lport_id);
    max_queue(udp_low_enabled, udp_low_lport_id);
    max_queue(l2_enabled, l2_lport_id);

    if (app_config.application_num_mbufs == 0)
        app_config.application_num_mbufs = DEFAULT_MBUF_COUNT;
    if (app_config.application_cache_size == 0)
        app_config.application_cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE / 2;
    else if (app_config.application_cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE)
        app_config.application_cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE / 2;
    if (app_config.application_mbuf_size == 0)
        app_config.application_mbuf_size = RTE_MBUF_DEFAULT_DATAROOM;
    else
        app_config.application_mbuf_size += RTE_PKTMBUF_HEADROOM;

    if (app_config.application_num_rx_descriptors == 0)
        app_config.application_num_rx_descriptors = NUM_RX_DESC_DEFAULT;
    if (app_config.application_num_tx_descriptors == 0)
        app_config.application_num_tx_descriptors = NUM_TX_DESC_DEFAULT;

    if (app_config.application_link_speed == 0)
        app_config.application_link_speed = RTE_ETH_SPEED_NUM_UNKNOWN;
    if (app_config.application_wait_time_before_start < MIN_WAIT_TIME)
        app_config.application_wait_time_before_start = MIN_WAIT_TIME;
    /*
     * Re-calculate default base start time. There is one case where this necessary:
     *  - The user provided a different clock_id than TAI in yaml file
     *  - The user did not provide a base time in yaml file
     *
     * In that case the default base time calculated by config_set_defaults() is based on
     * TAI. That has to be re-done by using the user provided clock id.
     */
    if (app_config.application_clock_id != CLOCK_TAI && !base_time_seen) {
        struct timespec current;

        clock_gettime(app_config.application_clock_id, &current);
    }

    ret = 0;

err_parse:
    yaml_token_delete(&token);
    yaml_parser_delete(&parser);

err_yaml:
    fclose(f);

    return ret;
}

static void
application_config(void)
{
    printf("   ClockId=%s\n",
           app_config.application_clock_id == CLOCK_TAI ? "CLOCK_TAI" : "CLOCK_MONOTONIC");
    printf("   BaseCycleTimeNS=%'" PRIu64 "\n", app_config.application_base_cycle_time_ns);
    printf("   TxBaseOffsetNS=%'" PRIu64 "\n", app_config.application_tx_base_offset_ns);
    printf("   RxBaseOffsetNS=%'" PRIu64 "\n", app_config.application_rx_base_offset_ns);
    printf("   NumMbufs=%'u\n", app_config.application_num_mbufs);
    printf("   MbufSize=%'u\n", app_config.application_mbuf_size);
    printf("   CacheSize=%'u\n", app_config.application_cache_size);
    printf("   NumRxDescriptors=%'u\n", app_config.application_num_rx_descriptors);
    printf("   NumTxDescriptors=%'u\n", app_config.application_num_tx_descriptors);
    if (app_config.application_link_speed == RTE_ETH_SPEED_NUM_UNKNOWN)
        printf("   LinkSpeed=%s\n", "AutoNeg");
    else
        printf("   LinkSpeed=%'dMbps\n", app_config.application_link_speed);
    printf("   LinkHalfDuplex=%s\n", app_config.application_link_half_duplex ? "Half" : "Full");
    printf("   WaitTimeBeforeStart=%'u\n", app_config.application_wait_time_before_start);
}

static void
pkt_handler_config(void)
{
    if (app_config.pkt_handler_enabled) {
        printf("   Enabled=%s\n", app_config.pkt_handler_enabled ? "True" : "False");
        printf("   LPortID=%s\n", lport_format(app_config.pkt_handler_lport_id));
        printf("   ThreadCpu=%d\n", app_config.pkt_handler_thread_cpu);
    } else
        printf("   ** Disabled **\n");
}

static void
tsn_high_config(void)
{
    if (app_config.tsn_high_enabled) {
        printf("   Enabled=%s\n", app_config.tsn_high_enabled ? "True" : "False");
        printf("   TxTimeEnabled=%s\n", app_config.tsn_high_tx_time_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.tsn_high_ignore_rx_errors ? "True" : "False");
        printf("   TxTimeOffsetNS=%" PRIu64 "\n", app_config.tsn_high_tx_time_offset_ns);
        printf("   Vid=%d\n", app_config.tsn_high_vid);
        printf("   Pcp=%d\n", app_config.tsn_high_pcp);
        printf("   NumFramesPerCycle=%zu\n", app_config.tsn_high_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.tsn_high_payload_pattern,
                              app_config.tsn_high_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.tsn_high_frame_length);
        printf("   SecurityMode=%s\n", security_mode_to_string(app_config.tsn_high_security_mode));
        printf("   SecurityAlgorithm=%s\n",
               security_algorithm_to_string(app_config.tsn_high_security_algorithm));
        printf("   SecurityKey=%s\n", app_config.tsn_high_security_key);
        printf("   SecurityIvPrefix=%s\n", app_config.tsn_high_security_iv_prefix);
        printf("   LPortID=%s\n", lport_format(app_config.tsn_high_lport_id));
        printf("   ThreadCpu=%d\n", app_config.tsn_high_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.tsn_high_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
tsn_low_config(void)
{
    if (app_config.tsn_low_enabled) {
        printf("   Enabled=%s\n", app_config.tsn_low_enabled ? "True" : "False");
        printf("   TxTimeEnabled=%s\n", app_config.tsn_low_tx_time_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.tsn_low_ignore_rx_errors ? "True" : "False");
        printf("   TxTimeOffsetNS=%" PRIu64 "\n", app_config.tsn_low_tx_time_offset_ns);
        printf("   Vid=%d\n", app_config.tsn_low_vid);
        printf("   Pcp=%d\n", app_config.tsn_low_pcp);
        printf("   NumFramesPerCycle=%zu\n", app_config.tsn_low_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.tsn_low_payload_pattern,
                              app_config.tsn_low_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.tsn_low_frame_length);
        printf("   SecurityMode=%s\n", security_mode_to_string(app_config.tsn_low_security_mode));
        printf("   SecurityAlgorithm=%s\n",
               security_algorithm_to_string(app_config.tsn_low_security_algorithm));
        printf("   SecurityKey=%s\n", app_config.tsn_low_security_key);
        printf("   SecurityIvPrefix=%s\n", app_config.tsn_low_security_iv_prefix);
        printf("   LPortID=%s\n", lport_format(app_config.tsn_low_lport_id));
        printf("   ThreadCpu=%d\n", app_config.tsn_low_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.tsn_high_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
rtc_config(void)
{
    if (app_config.rtc_enabled) {
        printf("   Enabled=%s\n", app_config.rtc_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.rtc_ignore_rx_errors ? "True" : "False");
        printf("   Vid=%d\n", app_config.rtc_vid);
        printf("   Pcp=%d\n", app_config.rtc_pcp);
        printf("   NumFramesPerCycle=%zu\n", app_config.rtc_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.rtc_payload_pattern,
                              app_config.rtc_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.rtc_frame_length);
        printf("   SecurityMode=%s\n", security_mode_to_string(app_config.rtc_security_mode));
        printf("   SecurityAlgorithm=%s\n",
               security_algorithm_to_string(app_config.rtc_security_algorithm));
        printf("   SecurityKey=%s\n", app_config.rtc_security_key);
        printf("   SecurityIvPrefix=%s\n", app_config.rtc_security_iv_prefix);
        printf("   LPortID=%s\n", lport_format(app_config.rtc_lport_id));
        printf("   ThreadCpu=%d\n", app_config.rtc_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.rtc_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
rta_config(void)
{
    if (app_config.rta_enabled) {
        printf("   Enabled=%s\n", app_config.rta_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.rta_ignore_rx_errors ? "True" : "False");
        printf("   Vid=%d\n", app_config.rta_vid);
        printf("   Pcp=%d\n", app_config.rta_pcp);
        printf("   BurstPeriodNS=%'" PRIu64 "\n", app_config.rta_burst_period_ns);
        printf("   NumFramesPerCycle=%zu\n", app_config.rta_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.rta_payload_pattern,
                              app_config.rta_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.rta_frame_length);
        printf("   SecurityMode=%s\n", security_mode_to_string(app_config.rta_security_mode));
        printf("   SecurityAlgorithm=%s\n",
               security_algorithm_to_string(app_config.rta_security_algorithm));
        printf("   SecurityKey=%s\n", app_config.rta_security_key);
        printf("   SecurityIvPrefix=%s\n", app_config.rta_security_iv_prefix);
        printf("   LPortID=%s\n", lport_format(app_config.rta_lport_id));
        printf("   ThreadCpu=%d\n", app_config.rta_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.rta_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
dcp_config(void)
{
    if (app_config.dcp_enabled) {
        printf("   Enabled=%s\n", app_config.dcp_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.dcp_ignore_rx_errors ? "True" : "False");
        printf("   Vid=%d\n", app_config.dcp_vid);
        printf("   Pcp=%d\n", app_config.dcp_pcp);
        printf("   BurstPeriodNS=%'" PRIu64 "\n", app_config.dcp_burst_period_ns);
        printf("   NumFramesPerCycle=%zu\n", app_config.dcp_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.dcp_payload_pattern,
                              app_config.dcp_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.dcp_frame_length);
        printf("   LPortID=%s\n", lport_format(app_config.dcp_lport_id));
        printf("   ThreadCpu=%d\n", app_config.dcp_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.dcp_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
lldp_config(void)
{
    if (app_config.lldp_enabled) {
        printf("   Enabled=%s\n", app_config.lldp_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.dcp_ignore_rx_errors ? "True" : "False");
        printf("   BurstPeriodNS=%'" PRIu64 "\n", app_config.lldp_burst_period_ns);
        printf("   NumFramesPerCycle=%zu\n", app_config.lldp_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.lldp_payload_pattern,
                              app_config.lldp_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.lldp_frame_length);
        printf("   LPortID=%s\n", lport_format(app_config.lldp_lport_id));
        printf("   ThreadCpu=%d\n", app_config.lldp_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.lldp_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
udp_high_config(void)
{
    if (app_config.udp_high_enabled) {
        printf("   Enabled=%s\n", app_config.udp_high_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.udp_high_ignore_rx_errors ? "True" : "False");
        printf("   BurstPeriodNS=%'" PRIu64 "\n", app_config.udp_high_burst_period_ns);
        printf("   NumFramesPerCycle=%zu\n", app_config.udp_high_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.udp_high_payload_pattern,
                              app_config.udp_high_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.udp_high_frame_length);
        printf("   LPortID=%s\n", lport_format(app_config.udp_high_lport_id));
        printf("   ThreadCpu=%d\n", app_config.udp_high_thread_cpu);
        printf("   DstPort=%d\n", app_config.udp_high_port);
        printf("   SrcPort=%d\n", app_config.udp_high_src_port);
        printf("   Destination=%s\n", app_config.udp_high_destination);
        printf("   Source=%s\n", app_config.udp_high_source);
        printf("   Mac Destination=");
        print_mac_address((const unsigned char *)app_config.udp_high_mac_destination);
    } else
        printf("   ** Disabled **\n");
}

static void
udp_low_config(void)
{
    if (app_config.udp_low_enabled) {
        printf("   Enabled=%s\n", app_config.udp_low_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.udp_low_ignore_rx_errors ? "True" : "False");
        printf("   BurstPeriodNS=%'" PRIu64 "\n", app_config.udp_low_burst_period_ns);
        printf("   NumFramesPerCycle=%zu\n", app_config.udp_low_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.udp_low_payload_pattern,
                              app_config.udp_low_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.udp_low_frame_length);
        printf("   LPortID=%s\n", lport_format(app_config.udp_low_lport_id));
        printf("   ThreadCpu=%d\n", app_config.udp_low_thread_cpu);
        printf("   DstPort=%d\n", app_config.udp_low_port);
        printf("   SrcPort=%d\n", app_config.udp_low_src_port);
        printf("   Destination=%s\n", app_config.udp_low_destination);
        printf("   Source=%s\n", app_config.udp_low_source);
        printf("   Mac Destination=");
        print_mac_address((const unsigned char *)app_config.udp_low_mac_destination);
    } else
        printf("   ** Disabled **\n");
}

static void
l2_config(void)
{
    if (app_config.l2_enabled) {
        printf("   Enabled=%s\n", app_config.l2_enabled ? "True" : "False");
        printf("   TxTimeEnabled=%s\n", app_config.l2_tx_time_enabled ? "True" : "False");
        printf("   IgnoreRxErrors=%s\n", app_config.l2_ignore_rx_errors ? "True" : "False");
        printf("   TxTimeOffsetNS=%" PRIu64 "\n", app_config.l2_tx_time_offset_ns);
        printf("   Vid=%d\n", app_config.l2_vid);
        printf("   Pcp=%d\n", app_config.l2_pcp);
        printf("   EtherType=0x%04x\n", app_config.l2_ether_type);
        printf("   NumFramesPerCycle=%zu\n", app_config.l2_num_frames_per_cycle);
        printf("   PayloadPattern=");
        print_payload_pattern(app_config.l2_payload_pattern, app_config.l2_payload_pattern_length);
        printf("\n");
        printf("   FrameLength=%zu\n", app_config.l2_frame_length);
        printf("   LPortID=%s\n", lport_format(app_config.l2_lport_id));
        printf("   ThreadCpu=%d\n", app_config.l2_thread_cpu);
        printf("   Destination=");
        print_mac_address(app_config.l2_destination);
        printf("\n");
    } else
        printf("   ** Disabled **\n");
}

static void
log_config(void)
{
    printf("   ThreadPeriodNS=%" PRIu64 "\n", app_config.log_thread_period_ns);
    printf("   ThreadPriority=%d\n", app_config.log_thread_priority);
    printf("   ThreadCpu=%d\n", app_config.log_thread_cpu);
    printf("   File=%s\n", app_config.log_file);
    printf("   Level=%s\n", app_config.log_level);
}

static void
debug_config(void)
{
    printf("   StopTraceOnOutlier=%s\n", app_config.debug_stop_trace_on_outlier ? "True" : "False");
    printf("   StopTraceOnError=%s\n", app_config.debug_stop_trace_on_error ? "True" : "False");
    printf("   MonitorMode=%s\n", app_config.debug_monitor_mode ? "True" : "False");
    printf("   MonitorDestination=");
    print_mac_address(app_config.debug_monitor_destination);
    printf("\n");
}

static void
stats_histogram_config(void)
{
    if (app_config.stats_histogram_enabled) {
        printf("   HistogramEnabled=%s\n", app_config.stats_histogram_enabled ? "True" : "False");
        printf("   HistogramMinimumNS=%" PRIu64 "\n", app_config.stats_histogram_minimum_ns);
        printf("   HistogramMaximumNS=%" PRIu64 "\n", app_config.stats_histogram_maximum_ns);
        printf("   HistogramFile=%s\n", app_config.stats_histogram_file);
        printf("   CollectionIntervalNS=%" PRIu64 "\n", app_config.stats_collection_interval_ns);
    } else
        printf("   ** Disabled **\n");
}

static void
log_mqtt_config(void)
{
    if (app_config.log_via_mqtt) {
        printf("   MQTT=%s\n", app_config.log_via_mqtt ? "True" : "False");
        printf("   MQTTThreadPriority=%d\n", app_config.log_via_mqtt_thread_priority);
        printf("   MQTTThreadCpu=%d\n", app_config.log_via_mqtt_thread_cpu);
        printf("   MQTTThreadPeriodNS=%" PRIu64 "\n", app_config.log_via_mqtt_thread_period_ns);
        printf("   MQTTBrokerIP=%s\n", app_config.log_via_mqtt_broker_ip);
        printf("   MQTTBrokerPort=%d\n", app_config.log_via_mqtt_broker_port);
        printf("   MQTTKeepAliveSecs=%d\n", app_config.log_via_mqtt_keep_alive_secs);
        printf("   MQTTMeasurementName=%s\n", app_config.log_via_mqtt_measurement_name);
    } else
        printf("   ** Disabled **\n");
}

static inline void
print_config(const char *desc, void (*fn)(void))
{
    printf(">> %-30s -----------------------------------------------\n", desc);
    fn();
}

void
config_print_values(void)
{
    // clang-format off
	struct {
		const char *desc;
		void (*config_fn)(void);
	} config_descs[] = {
		{"Application", application_config},
		{"Packet Handler 'PktHandler'", pkt_handler_config},
		{"TSN High 'TsnHigh'", tsn_high_config},
		{"TSN Low 'TsnLow'", tsn_low_config},
        {"RTC 'Rtc'", rtc_config},
		{"RTA 'Rta'", rta_config},
		{"DCP 'Dcp'", dcp_config},
		{"LLDP 'Lldp'", lldp_config},
		{"UDP High 'UdpHigh'", udp_high_config},
		{"UDP Low 'UdpLow'", udp_low_config},
		{"Generic L2 'L2'", l2_config},
		{"Log", log_config},
		{"Debug", debug_config},
		{"Statistic Histogram 'Stats'", stats_histogram_config},
		{"Log via MQTT 'LogVia'", log_mqtt_config}
	};
    // clang-format on

    for (int i = 0; i < (int)ARRAY_SIZE(config_descs); i++)
        print_config(config_descs[i].desc, config_descs[i].config_fn);
    printf("---------------------------------------------------------------------------------"
           "\n");
}

static int
config_set_defaults(void)
{
    static unsigned char default_debug_monitor_destination[] = {0x44, 0x44, 0x44, 0x44, 0x44, 0x44};
    static unsigned char default_lldp_destination[]          = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};
    static unsigned char default_udp_destination[]           = {0xa4, 0xa2, 0xc2, 0x00, 0x00, 0xee};
    static unsigned char default_destination[]               = {0xa8, 0xa1, 0x59, 0x2c, 0xa8, 0xdb};
    static unsigned char default_dcp_identify[]              = {0x01, 0x0e, 0xcf, 0x00, 0x00, 0x00};
    static const char *default_log_via_mqtt_measurement_name = "testbench";
    static const char *default_udp_high_destination          = "192.168.2.121";
    static const char *default_udp_high_source               = "192.168.2.120";
    static const char *default_udp_low_destination           = "192.168.2.121";
    static const char *default_udp_low_source                = "192.168.2.120";
    static const char *default_log_via_mqtt_broker_ip        = "127.0.0.1";
    static const char *default_payload_pattern               = "Payload";
    static const char *default_hist_file                     = "histogram.txt";
    uint16_t default_udp_high_port                           = 6666;
    uint16_t default_udp_low_port                            = 6667;
    static const char *default_log_level                     = "Debug";
    // struct timespec current;
    int ret = -ENOMEM;

    // clock_gettime(CLOCK_TAI, &current);

    /* Application scheduling configuration */
    app_config.application_clock_id               = CLOCK_TAI;
    app_config.application_base_cycle_time_ns     = 125000;
    app_config.application_tx_base_offset_ns      = 100000;
    app_config.application_rx_base_offset_ns      = 75000;
    app_config.application_num_mbufs              = 4096;
    app_config.application_mbuf_size              = 2048;
    app_config.application_cache_size             = 32;
    app_config.application_num_rx_descriptors     = 0;
    app_config.application_num_tx_descriptors     = 0;
    app_config.application_link_speed             = RTE_ETH_SPEED_NUM_UNKNOWN;
    app_config.application_link_half_duplex       = false;
    app_config.application_wait_time_before_start = MIN_WAIT_TIME;

    /* Packet Handler */
    app_config.pkt_handler_enabled           = false;
    app_config.pkt_handler_lport_id          = LPORT_INVALID;
    app_config.pkt_handler_physical_lport_id = LPORT_INVALID;
    app_config.pkt_handler_thread_cpu        = -1;

    /* TSN High */
    app_config.tsn_high_enabled              = false;
    app_config.tsn_high_tx_time_enabled      = false;
    app_config.tsn_high_ignore_rx_errors     = false;
    app_config.tsn_high_tx_time_offset_ns    = 0;
    app_config.tsn_high_vid                  = TSN_HIGH_VID_VALUE;
    app_config.tsn_high_pcp                  = TSN_HIGH_PCP_VALUE;
    app_config.tsn_high_num_frames_per_cycle = 0;
    app_config.tsn_high_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.tsn_high_payload_pattern)
        goto out;
    app_config.tsn_high_payload_pattern_length = strlen(app_config.tsn_high_payload_pattern);
    app_config.tsn_high_frame_length           = 200;
    app_config.tsn_high_security_mode          = SECURITY_MODE_NONE;
    app_config.tsn_high_security_algorithm     = SECURITY_ALGORITHM_AES256_GCM;
    app_config.tsn_high_security_key           = NULL;
    app_config.tsn_high_security_iv_prefix     = NULL;
    app_config.tsn_high_lport_id               = LPORT_INVALID;
    app_config.tsn_high_thread_cpu             = -1;
    memcpy((void *)app_config.tsn_high_destination, default_destination, ETH_ALEN);

    /* TSN Low */
    app_config.tsn_low_enabled              = false;
    app_config.tsn_low_tx_time_enabled      = false;
    app_config.tsn_low_tx_time_offset_ns    = 0;
    app_config.tsn_low_vid                  = TSN_LOW_VID_VALUE;
    app_config.tsn_low_pcp                  = TSN_LOW_PCP_VALUE;
    app_config.tsn_low_num_frames_per_cycle = 0;
    app_config.tsn_low_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.tsn_low_payload_pattern)
        goto out;
    app_config.tsn_low_payload_pattern_length = strlen(app_config.tsn_low_payload_pattern);
    app_config.tsn_low_frame_length           = 200;
    app_config.tsn_low_security_mode          = SECURITY_MODE_NONE;
    app_config.tsn_low_security_algorithm     = SECURITY_ALGORITHM_AES256_GCM;
    app_config.tsn_low_security_key           = NULL;
    app_config.tsn_low_security_iv_prefix     = NULL;
    app_config.tsn_low_lport_id               = LPORT_INVALID;
    app_config.tsn_low_thread_cpu             = -1;
    memcpy((void *)app_config.tsn_low_destination, default_destination, ETH_ALEN);

    /* Real Time Cyclic (RTC) */
    app_config.rtc_enabled              = false;
    app_config.rtc_ignore_rx_errors     = false;
    app_config.rtc_vid                  = PROFINET_RT_VID_VALUE;
    app_config.rtc_pcp                  = RTC_PCP_VALUE;
    app_config.rtc_num_frames_per_cycle = 0;
    app_config.rtc_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.rtc_payload_pattern)
        goto out;
    app_config.rtc_payload_pattern_length = strlen(app_config.rtc_payload_pattern);
    app_config.rtc_frame_length           = 200;
    app_config.rtc_security_mode          = SECURITY_MODE_NONE;
    app_config.rtc_security_algorithm     = SECURITY_ALGORITHM_AES256_GCM;
    app_config.rtc_security_key           = NULL;
    app_config.rtc_security_iv_prefix     = NULL;
    app_config.rtc_lport_id               = LPORT_INVALID;
    app_config.rtc_thread_cpu             = -1;
    memcpy((void *)app_config.rtc_destination, default_destination, ETH_ALEN);

    /* Real Time Acyclic (RTA) */
    app_config.rta_enabled              = false;
    app_config.rta_ignore_rx_errors     = false;
    app_config.rta_vid                  = PROFINET_RT_VID_VALUE;
    app_config.rta_pcp                  = RTA_PCP_VALUE;
    app_config.rta_burst_period_ns      = 200000000;
    app_config.rta_num_frames_per_cycle = 0;
    app_config.rta_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.rta_payload_pattern)
        goto out;
    app_config.rta_payload_pattern_length = strlen(app_config.rta_payload_pattern);
    app_config.rta_frame_length           = 200;
    app_config.rta_security_mode          = SECURITY_MODE_NONE;
    app_config.rta_security_algorithm     = SECURITY_ALGORITHM_AES256_GCM;
    app_config.rta_security_key           = NULL;
    app_config.rta_security_iv_prefix     = NULL;
    app_config.rta_lport_id               = LPORT_INVALID;
    app_config.rta_thread_cpu             = -1;
    memcpy((void *)app_config.rta_destination, default_destination, ETH_ALEN);

    /* Discovery and Configuration Protocol (DCP) */
    app_config.dcp_enabled              = false;
    app_config.dcp_ignore_rx_errors     = false;
    app_config.dcp_vid                  = PROFINET_RT_VID_VALUE;
    app_config.dcp_pcp                  = DCP_PCP_VALUE;
    app_config.dcp_burst_period_ns      = 2000000000;
    app_config.dcp_num_frames_per_cycle = 0;
    app_config.dcp_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.dcp_payload_pattern)
        goto out;
    app_config.dcp_payload_pattern_length = strlen(app_config.dcp_payload_pattern);
    app_config.dcp_frame_length           = 200;
    app_config.dcp_lport_id               = LPORT_INVALID;
    app_config.dcp_thread_cpu             = -1;
    memcpy((void *)app_config.dcp_destination, default_dcp_identify, ETH_ALEN);

    /* Link Layer Discovery Protocol (LLDP) */
    app_config.lldp_enabled              = false;
    app_config.lldp_ignore_rx_errors     = false;
    app_config.lldp_burst_period_ns      = 5000000000;
    app_config.lldp_num_frames_per_cycle = 0;
    app_config.lldp_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.lldp_payload_pattern)
        goto out;
    app_config.lldp_payload_pattern_length = strlen(app_config.lldp_payload_pattern);
    app_config.lldp_frame_length           = 200;
    app_config.lldp_lport_id               = LPORT_INVALID;
    app_config.lldp_thread_cpu             = -1;
    memcpy((void *)app_config.lldp_destination, default_lldp_destination, ETH_ALEN);

    /* User Datagram Protocol (UDP) High */
    app_config.udp_high_enabled              = false;
    app_config.udp_high_ignore_rx_errors     = false;
    app_config.udp_high_burst_period_ns      = 1000000000;
    app_config.udp_high_num_frames_per_cycle = 0;
    app_config.udp_high_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.udp_high_payload_pattern)
        goto out;
    app_config.udp_high_payload_pattern_length = strlen(app_config.udp_high_payload_pattern);
    app_config.udp_high_frame_length           = 1400;
    app_config.udp_high_lport_id               = LPORT_INVALID;
    app_config.udp_high_thread_cpu             = -1;
    app_config.udp_high_port                   = default_udp_high_port;
    if (!app_config.udp_high_port)
        goto out;
    app_config.udp_high_destination = strdup(default_udp_high_destination);
    if (!app_config.udp_high_destination)
        goto out;
    app_config.udp_high_source = strdup(default_udp_high_source);
    if (!app_config.udp_high_source)
        goto out;
    memcpy((void *)app_config.udp_high_mac_destination, default_udp_destination, ETH_ALEN);

    /* User Datagram Protocol (UDP) Low */
    app_config.udp_low_enabled              = false;
    app_config.udp_low_ignore_rx_errors     = false;
    app_config.udp_low_burst_period_ns      = 1000000000;
    app_config.udp_low_num_frames_per_cycle = 0;
    app_config.udp_low_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.udp_low_payload_pattern)
        goto out;
    app_config.udp_low_payload_pattern_length = strlen(app_config.udp_low_payload_pattern);
    app_config.udp_low_frame_length           = 1400;
    app_config.udp_low_lport_id               = LPORT_INVALID;
    app_config.udp_low_thread_cpu             = -1;
    app_config.udp_low_port                   = default_udp_low_port;
    if (!app_config.udp_low_port)
        goto out;
    app_config.udp_low_destination = strdup(default_udp_low_destination);
    if (!app_config.udp_low_destination)
        goto out;
    app_config.udp_low_source = strdup(default_udp_low_source);
    if (!app_config.udp_low_source)
        goto out;
    memcpy((void *)app_config.udp_low_mac_destination, default_udp_destination, ETH_ALEN);

    app_config.l2_enabled              = false;
    app_config.l2_tx_time_enabled      = false;
    app_config.l2_ignore_rx_errors     = false;
    app_config.l2_tx_time_offset_ns    = 0;
    app_config.l2_vid                  = 100;
    app_config.l2_pcp                  = 6;
    app_config.l2_ether_type           = 0xb62c;
    app_config.l2_num_frames_per_cycle = 0;
    app_config.l2_payload_pattern      = strdup(default_payload_pattern);
    if (!app_config.l2_payload_pattern)
        goto out;
    app_config.l2_payload_pattern_length = strlen(app_config.l2_payload_pattern);
    app_config.l2_frame_length           = 200;
    app_config.l2_lport_id               = LPORT_INVALID;
    app_config.l2_thread_cpu             = -1;
    memcpy((void *)app_config.l2_destination, default_destination, ETH_ALEN);

    /* Logging */
    app_config.log_thread_period_ns = 500000000;
    app_config.log_thread_priority  = 1;
    app_config.log_thread_cpu       = 7;
    app_config.log_file             = strdup("reference.log");
    if (!app_config.log_file)
        goto out;
    app_config.log_level = strdup(default_log_level);
    if (!app_config.log_level)
        goto out;

    /* Debug */
    app_config.debug_stop_trace_on_outlier = false;
    app_config.debug_stop_trace_on_error   = false;
    app_config.debug_monitor_mode          = false;
    memcpy((void *)app_config.debug_monitor_destination, default_debug_monitor_destination,
           ETH_ALEN);

    /* Stats */
    app_config.stats_histogram_enabled    = false;
    app_config.stats_histogram_minimum_ns = 1 * 1e6;
    app_config.stats_histogram_maximum_ns = 10 * 1e6;
    app_config.stats_histogram_file       = strdup(default_hist_file);
    if (!app_config.stats_histogram_file)
        goto out;
    app_config.stats_histogram_file_length  = strlen(default_hist_file);
    app_config.stats_collection_interval_ns = 1e9;

    /* LogViaMQTT */
    app_config.log_via_mqtt                  = false;
    app_config.log_via_mqtt_broker_port      = 1883;
    app_config.log_via_mqtt_thread_priority  = 1;
    app_config.log_via_mqtt_thread_cpu       = 7;
    app_config.log_via_mqtt_keep_alive_secs  = 60;
    app_config.log_via_mqtt_thread_period_ns = 1e9;
    app_config.log_via_mqtt_broker_ip        = strdup(default_log_via_mqtt_broker_ip);
    if (!app_config.log_via_mqtt_broker_ip)
        goto out;

    app_config.log_via_mqtt_measurement_name = strdup(default_log_via_mqtt_measurement_name);
    if (!app_config.log_via_mqtt_measurement_name)
        goto out;

    return 0;
out:
    __config_free();
    return ret;
}

static bool
config_check_keys(const char *traffic_class, enum security_mode mode,
                  enum security_algorithm algorithm, size_t key_len, size_t iv_prefix_len)
{
    const size_t expected_key_len = algorithm == SECURITY_ALGORITHM_AES128_GCM ? 16 : 32;

    if (mode == SECURITY_MODE_NONE)
        return true;

    if (iv_prefix_len != SECURITY_IV_PREFIX_LEN) {
        fprintf(stderr, "%s IV prefix length should be %d!\n", traffic_class,
                SECURITY_IV_PREFIX_LEN);
        return false;
    }

    if (expected_key_len != key_len) {
        fprintf(stderr, "%s key length mismatch!. Have %zu expected %zu for %s!\n", traffic_class,
                key_len, expected_key_len, security_algorithm_to_string(algorithm));
        return false;
    }

    return true;
}

bool
config_sanity_check(void)
{
    const size_t min_secure_profinet_frame_size = sizeof(struct vlan_ethernet_header) +
                                                  sizeof(struct profinet_secure_header) +
                                                  sizeof(struct security_checksum);
    const size_t min_profinet_frame_size =
        sizeof(struct vlan_ethernet_header) + sizeof(struct profinet_rt_header);
    size_t min_frame_size;

    /*
     * Perform configuration sanity checks. This includes:
     *   - Traffic classes
     *   - Frame lengths
     *   - Limitations
     */

    /* Either L2 or PROFINET should be active. */
    if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(l2) &&
        (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_high) || CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rtc) ||
         CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rta) || CONFIG_IS_TRAFFIC_CLASS_ACTIVE(dcp) ||
         CONFIG_IS_TRAFFIC_CLASS_ACTIVE(lldp) || CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_high) ||
         CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_low))) {
        fprintf(stderr, "Either use PROFINET or L2!\n");
        fprintf(stderr, "For simulation of PROFINET and other middlewares in parallel "
                        "start multiple instances of ref&mirror application(s) with "
                        "different profiles!\n");
        return false;
    }

    /* Frame lengths */
    if (app_config.l2_frame_length > MAX_FRAME_SIZE ||
        app_config.l2_frame_length <
            (sizeof(struct vlan_ethernet_header) + sizeof(struct l2_header) +
             app_config.l2_payload_pattern_length)) {
        fprintf(stderr, "L2FrameLength is invalid!\n");
        return false;
    }

    min_frame_size = app_config.tsn_high_security_mode == SECURITY_MODE_NONE
                         ? min_profinet_frame_size
                         : min_secure_profinet_frame_size;
    if (app_config.tsn_high_frame_length > MAX_FRAME_SIZE ||
        app_config.tsn_high_frame_length <
            (min_frame_size + app_config.tsn_high_payload_pattern_length)) {
        fprintf(stderr, "TsnHighFrameLength is invalid!\n");
        return false;
    }

    min_frame_size = app_config.tsn_low_security_mode == SECURITY_MODE_NONE
                         ? min_profinet_frame_size
                         : min_secure_profinet_frame_size;
    if (app_config.tsn_low_frame_length > MAX_FRAME_SIZE ||
        app_config.tsn_low_frame_length <
            (min_frame_size + app_config.tsn_low_payload_pattern_length)) {
        fprintf(stderr, "TsnLowFrameLength is invalid!\n");
        return false;
    }

    min_frame_size = app_config.rtc_security_mode == SECURITY_MODE_NONE
                         ? min_profinet_frame_size
                         : min_secure_profinet_frame_size;
    if (app_config.rtc_frame_length > MAX_FRAME_SIZE ||
        app_config.rtc_frame_length < (min_frame_size + app_config.rtc_payload_pattern_length)) {
        fprintf(stderr, "RtcFrameLength is invalid!\n");
        return false;
    }

    min_frame_size = app_config.rta_security_mode == SECURITY_MODE_NONE
                         ? min_profinet_frame_size
                         : min_secure_profinet_frame_size;
    if (app_config.rta_frame_length > MAX_FRAME_SIZE ||
        app_config.rta_frame_length < (min_frame_size + app_config.rta_payload_pattern_length)) {
        fprintf(stderr, "RtaFrameLength is invalid!\n");
        return false;
    }

    if (app_config.dcp_frame_length > MAX_FRAME_SIZE ||
        app_config.dcp_frame_length <
            (min_profinet_frame_size + app_config.dcp_payload_pattern_length)) {
        fprintf(stderr, "DcpFrameLength is invalid!\n");
        return false;
    }

    if (app_config.lldp_frame_length > MAX_FRAME_SIZE ||
        app_config.lldp_frame_length < (sizeof(struct ethhdr) + sizeof(struct reference_meta_data) +
                                        app_config.lldp_payload_pattern_length)) {
        fprintf(stderr, "LldpFrameLength is invalid!\n");
        return false;
    }

    if (app_config.udp_high_frame_length > MAX_FRAME_SIZE ||
        app_config.udp_high_frame_length <
            (sizeof(struct reference_meta_data) + app_config.udp_high_payload_pattern_length)) {
        fprintf(stderr, "UdpHighFrameLength is invalid!\n");
        return false;
    }

    if (app_config.udp_low_frame_length > MAX_FRAME_SIZE ||
        app_config.udp_low_frame_length <
            (sizeof(struct reference_meta_data) + app_config.udp_low_payload_pattern_length)) {
        fprintf(stderr, "UdpLowFrameLength is invalid!\n");
        return false;
    }

    /* XDP busy polling only works beginning with Linux kernel version v5.11 */
    if (!config_have_busy_poll()) {
        fprintf(stderr, "XDP busy polling selected, but not supported!\n");
        return false;
    }

    if (!config_have_mosquitto() && app_config.log_via_mqtt) {
        fprintf(stderr, "Log via Mosquito enabled, but not supported!\n");
        return false;
    }

    /* Check keys and IV */
    if (!config_check_keys(
            "TsnHigh", app_config.tsn_high_security_mode, app_config.tsn_high_security_algorithm,
            app_config.tsn_high_security_key_length, app_config.tsn_high_security_iv_prefix_length))
        return false;
    if (!config_check_keys(
            "TsnLow", app_config.tsn_low_security_mode, app_config.tsn_low_security_algorithm,
            app_config.tsn_low_security_key_length, app_config.tsn_low_security_iv_prefix_length))
        return false;
    if (!config_check_keys("Rtc", app_config.rtc_security_mode, app_config.rtc_security_algorithm,
                           app_config.rtc_security_key_length,
                           app_config.rtc_security_iv_prefix_length))
        return false;
    if (!config_check_keys("Rta", app_config.rta_security_mode, app_config.rta_security_algorithm,
                           app_config.rta_security_key_length,
                           app_config.rta_security_iv_prefix_length))
        return false;

    /* Stats */
    if (app_config.stats_histogram_minimum_ns > app_config.stats_histogram_maximum_ns) {
        fprintf(stderr, "Histogram minimum and maximum values are invalid!\n");
        return false;
    }

    return true;
}

static void
__config_free(void)
{
    free(app_config.tsn_high_payload_pattern);
    free(app_config.tsn_high_security_key);
    free(app_config.tsn_high_security_iv_prefix);

    free(app_config.tsn_low_payload_pattern);
    free(app_config.tsn_low_security_key);
    free(app_config.tsn_low_security_iv_prefix);

    free(app_config.rtc_payload_pattern);
    free(app_config.rtc_security_key);
    free(app_config.rtc_security_iv_prefix);

    free(app_config.rta_payload_pattern);
    free(app_config.rta_security_key);
    free(app_config.rta_security_iv_prefix);

    free(app_config.dcp_payload_pattern);

    free(app_config.lldp_payload_pattern);

    free(app_config.udp_high_payload_pattern);
    free(app_config.udp_high_destination);
    free(app_config.udp_high_source);

    free(app_config.udp_low_payload_pattern);
    free(app_config.udp_low_destination);
    free(app_config.udp_low_source);

    free(app_config.l2_payload_pattern);

    free(app_config.stats_histogram_file);

    free(app_config.log_file);
    free(app_config.log_level);

    free(app_config.log_via_mqtt_broker_ip);
    free(app_config.log_via_mqtt_measurement_name);
}

static int
config_init(void *arg __rte_unused)
{
    int ret = 0;

    fprintf(stderr, "Loading configuration from file %s...\n", config_file_path);

    memset(app_config.qinfo, 0, sizeof(app_config.qinfo));

    /*
     * The "mirror" application only mirrors traffic and never generate frames itself. Make
     * sure, the corresponding options are set.
     *
     * Note: The user cannot override this.
     */
    ret = config_set_defaults();
    if (ret) {
        fprintf(stderr, "Failed to set default config values!\n");
        return ret;
    }

    if (!config_file_path) {
        fprintf(stderr, "Specifying an configuration file is mandatory. See tests/ "
                        "directory for examples!\n");
        return -1;
    }

    ret = config_read_from_file(config_file_path);
    if (ret) {
        fprintf(stderr, "Failed to parse configuration file!\n");
        return ret;
    }
    config_print_values();

    return ret;
}

static int
config_launch(void *arg __rte_unused)
{
    return 0;
}

static void
config_deinit(void *arg __rte_unused)
{
    __config_free();
}

FUNCTION_REGISTER(config, CONFIG_IDX);

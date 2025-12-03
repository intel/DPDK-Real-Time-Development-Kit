/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <linux/if_ether.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "app_config.h"

#include "security.h"

extern bool enable_promiscuous;

#define MAX_QUEUES_ENABLED 16
typedef struct qid_s {
    uint16_t qids[MAX_QUEUES_ENABLED];        // Should equal MAX_NUM_QUEUES in lport.h
    uint16_t cnt;
} qid_t;

struct application_config {
    /* Application scheduling configuration */
    clockid_t application_clock_id;
    uint64_t application_base_cycle_time_ns;
    uint64_t application_tx_base_offset_ns;
    uint64_t application_rx_base_offset_ns;
    uint32_t application_num_mbufs;
    uint32_t application_mbuf_size;
    uint32_t application_cache_size;
    uint32_t application_num_rx_descriptors;
    uint32_t application_num_tx_descriptors;
    uint32_t application_link_speed;
    bool application_link_half_duplex;
    uint32_t application_wait_time_before_start;

    /* Packet Handler */
    bool pkt_handler_enabled;
    uint32_t pkt_handler_lport_id;
    uint32_t pkt_handler_physical_lport_id;
    int pkt_handler_thread_cpu;

    /* TSN High */
    bool tsn_high_enabled;
    bool tsn_high_tx_time_enabled;
    bool tsn_high_ignore_rx_errors;
    uint64_t tsn_high_tx_time_offset_ns;
    int tsn_high_vid;
    int tsn_high_pcp;
    size_t tsn_high_num_frames_per_cycle;
    char *tsn_high_payload_pattern;
    size_t tsn_high_payload_pattern_length;
    size_t tsn_high_frame_length;
    enum security_mode tsn_high_security_mode;
    enum security_algorithm tsn_high_security_algorithm;
    char *tsn_high_security_key;
    size_t tsn_high_security_key_length;
    char *tsn_high_security_iv_prefix;
    size_t tsn_high_security_iv_prefix_length;
    uint32_t tsn_high_lport_id;
    int tsn_high_thread_cpu;
    unsigned char tsn_high_destination[ETH_ALEN];

    /* TSN Low */
    bool tsn_low_enabled;
    bool tsn_low_tx_time_enabled;
    bool tsn_low_ignore_rx_errors;
    uint64_t tsn_low_tx_time_offset_ns;
    int tsn_low_vid;
    int tsn_low_pcp;
    size_t tsn_low_num_frames_per_cycle;
    char *tsn_low_payload_pattern;
    size_t tsn_low_payload_pattern_length;
    size_t tsn_low_frame_length;
    enum security_mode tsn_low_security_mode;
    enum security_algorithm tsn_low_security_algorithm;
    char *tsn_low_security_key;
    size_t tsn_low_security_key_length;
    char *tsn_low_security_iv_prefix;
    size_t tsn_low_security_iv_prefix_length;
    uint32_t tsn_low_lport_id;
    int tsn_low_thread_cpu;
    unsigned char tsn_low_destination[ETH_ALEN];

    /* Real Time Cyclic (RTC) */
    bool rtc_enabled;
    bool rtc_ignore_rx_errors;
    int rtc_vid;
    int rtc_pcp;
    size_t rtc_num_frames_per_cycle;
    char *rtc_payload_pattern;
    size_t rtc_payload_pattern_length;
    size_t rtc_frame_length;
    enum security_mode rtc_security_mode;
    enum security_algorithm rtc_security_algorithm;
    char *rtc_security_key;
    size_t rtc_security_key_length;
    char *rtc_security_iv_prefix;
    size_t rtc_security_iv_prefix_length;
    uint32_t rtc_lport_id;
    int rtc_thread_cpu;
    unsigned char rtc_destination[ETH_ALEN];

    /* Real Time Acyclic (RTA) */
    bool rta_enabled;
    bool rta_ignore_rx_errors;
    int rta_vid;
    int rta_pcp;
    uint64_t rta_burst_period_ns;
    size_t rta_num_frames_per_cycle;
    char *rta_payload_pattern;
    size_t rta_payload_pattern_length;
    size_t rta_frame_length;
    enum security_mode rta_security_mode;
    enum security_algorithm rta_security_algorithm;
    char *rta_security_key;
    size_t rta_security_key_length;
    char *rta_security_iv_prefix;
    size_t rta_security_iv_prefix_length;
    uint32_t rta_lport_id;
    int rta_thread_cpu;
    unsigned char rta_destination[ETH_ALEN];

    /* Discovery and Configuration Protocol (DCP) */
    bool dcp_enabled;
    bool dcp_ignore_rx_errors;
    int dcp_vid;
    int dcp_pcp;
    uint64_t dcp_burst_period_ns;
    size_t dcp_num_frames_per_cycle;
    char *dcp_payload_pattern;
    size_t dcp_payload_pattern_length;
    size_t dcp_frame_length;
    uint32_t dcp_lport_id;
    int dcp_thread_cpu;
    unsigned char dcp_destination[ETH_ALEN];

    /* Link Layer Discovery Protocol (LLDP) */
    bool lldp_enabled;
    bool lldp_ignore_rx_errors;
    uint64_t lldp_burst_period_ns;
    size_t lldp_num_frames_per_cycle;
    char *lldp_payload_pattern;
    size_t lldp_payload_pattern_length;
    size_t lldp_frame_length;
    uint32_t lldp_lport_id;
    int lldp_thread_cpu;
    unsigned char lldp_destination[ETH_ALEN];

    /* User Datagram Protocol (UDP) High */
    bool udp_high_enabled;
    bool udp_high_ignore_rx_errors;
    uint64_t udp_high_burst_period_ns;
    size_t udp_high_num_frames_per_cycle;
    char *udp_high_payload_pattern;
    size_t udp_high_payload_pattern_length;
    size_t udp_high_frame_length;
    uint32_t udp_high_lport_id;
    int udp_high_thread_cpu;
    uint16_t udp_high_port;
    uint16_t udp_high_src_port;
    char *udp_high_destination;
    size_t udp_high_destination_length;
    char *udp_high_source;
    size_t udp_high_source_length;
    char udp_high_mac_destination[ETH_ALEN];

    /* User Datagram Protocol (UDP) Low */
    bool udp_low_enabled;
    bool udp_low_ignore_rx_errors;
    uint64_t udp_low_burst_period_ns;
    size_t udp_low_num_frames_per_cycle;
    char *udp_low_payload_pattern;
    size_t udp_low_payload_pattern_length;
    size_t udp_low_frame_length;
    uint32_t udp_low_lport_id;
    int udp_low_thread_cpu;
    uint16_t udp_low_port;
    uint16_t udp_low_src_port;
    char *udp_low_destination;
    size_t udp_low_destination_length;
    char *udp_low_source;
    size_t udp_low_source_length;
    char udp_low_mac_destination[ETH_ALEN];

    /* Generic Layer 2 (example: OPC/UA PubSub) */
    bool l2_enabled;
    bool l2_tx_time_enabled;
    bool l2_ignore_rx_errors;
    uint64_t l2_tx_time_offset_ns;
    int l2_vid;
    int l2_pcp;
    unsigned int l2_ether_type;
    size_t l2_num_frames_per_cycle;
    char *l2_payload_pattern;
    size_t l2_payload_pattern_length;
    size_t l2_frame_length;
    uint32_t l2_lport_id;
    int l2_thread_cpu;
    unsigned char l2_destination[ETH_ALEN];

    /* Packet Mux */
    bool pmux_enabled;
    int pmux_thread_cpu;
    uint32_t pmux_lport_id;

    /* Logging */
    uint64_t log_thread_period_ns;
    int log_thread_priority;
    int log_thread_cpu;
    char *log_file;
    size_t log_file_length;
    char *log_level;
    size_t log_level_length;

    /* Debug */
    bool debug_stop_trace_on_outlier;
    bool debug_stop_trace_on_error;
    bool debug_monitor_mode;
    unsigned char debug_monitor_destination[ETH_ALEN];

    /* Statistics */
    bool stats_histogram_enabled;
    uint64_t stats_histogram_minimum_ns;
    uint64_t stats_histogram_maximum_ns;
    char *stats_histogram_file;
    size_t stats_histogram_file_length;
    uint64_t stats_collection_interval_ns;

    /* Log through MQTT */
    bool log_via_mqtt;
    int log_via_mqtt_thread_priority;
    int log_via_mqtt_thread_cpu;
    uint64_t log_via_mqtt_thread_period_ns;
    size_t log_via_mqtt_broker_ip_length;
    char *log_via_mqtt_broker_ip;
    int log_via_mqtt_broker_port;
    int log_via_mqtt_keep_alive_secs;
    size_t log_via_mqtt_measurement_name_length;
    char *log_via_mqtt_measurement_name;

    qid_t qinfo[RTE_MAX_ETHPORTS];
};

extern struct application_config app_config;

void config_set_file(const char *config_file);

int config_read_from_file(const char *config_file);
void config_print_values(void);
bool config_sanity_check(void);

#define CONFIG_STORE_BOOL_PARAM(name, var)                                      \
    do {                                                                        \
        if (!strcasecmp(key, #name)) {                                          \
            if (!strcmp(value, "0") || !strcasecmp(value, "false") ||           \
                !strcasecmp(value, "disabled") || !strcasecmp(value, "off"))    \
                app_config.var = false;                                         \
            else if (!strcmp(value, "1") || !strcasecmp(value, "true") ||       \
                     !strcasecmp(value, "enabled") || !strcasecmp(value, "on")) \
                app_config.var = true;                                          \
            else {                                                              \
                fprintf(stderr, "The value for " #name " is invalid!\n");       \
                goto err_parse;                                                 \
            }                                                                   \
        }                                                                       \
    } while (0)

#define CONFIG_STORE_INT_PARAM(name, var)                                 \
    do {                                                                  \
        if (!strcasecmp(key, #name)) {                                    \
            app_config.var = strtol(value, &endptr, 10);                  \
            if (errno != 0 || endptr == value || *endptr != '\0') {       \
                ret = -ERANGE;                                            \
                fprintf(stderr, "The value for " #name " is invalid!\n"); \
                goto err_parse;                                           \
            }                                                             \
        }                                                                 \
    } while (0)

#define CONFIG_STORE_UINT_PARAM(name, var)                                \
    do {                                                                  \
        if (!strcasecmp(key, #name)) {                                    \
            app_config.var = strtoul(value, &endptr, 10);                 \
            if (errno != 0 || endptr == value || *endptr != '\0') {       \
                ret = -ERANGE;                                            \
                fprintf(stderr, "The value for " #name " is invalid!\n"); \
                goto err_parse;                                           \
            }                                                             \
        }                                                                 \
    } while (0)

#define CONFIG_STORE_ULONG_PARAM(name, var)                                           \
    do {                                                                              \
        if (!strcasecmp(key, #name)) {                                                \
            app_config.var = strtoull(value, &endptr, 10);                            \
            if (errno != 0 || endptr == value || *endptr != '\0') {                   \
                ret = -ERANGE;                                                        \
                fprintf(stderr, "The value for " #name " is invalid (%d)!\n", errno); \
                goto err_parse;                                                       \
            }                                                                         \
        }                                                                             \
    } while (0)

#define CONFIG_STORE_STRING_PARAM(name, var)                          \
    do {                                                              \
        if (!strcasecmp(key, #name)) {                                \
            /* config_set_defaults() may have set a default value. */ \
            free(app_config.var);                                     \
            app_config.var = strdup(value);                           \
            if (!app_config.var) {                                    \
                ret = -ENOMEM;                                        \
                fprintf(stderr, "strdup() for " #name " failed!\n");  \
                goto err_parse;                                       \
            }                                                         \
            app_config.var##_length = strlen(value);                  \
        }                                                             \
    } while (0)

#define CONFIG_STORE_INTERFACE_PARAM(name, var)                         \
    do {                                                                \
        if (!strcasecmp(key, #name))                                    \
            strncpy(app_config.var, value, sizeof(app_config.var) - 1); \
    } while (0)

#define CONFIG_STORE_MAC_PARAM(name, var)                                                         \
    do {                                                                                          \
        if (!strcasecmp(key, #name)) {                                                            \
            unsigned int tmp[ETH_ALEN];                                                           \
            int i;                                                                                \
                                                                                                  \
            ret = sscanf(value, "%x:%x:%x:%x:%x:%x", &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], \
                         &tmp[5]);                                                                \
                                                                                                  \
            if (ret != ETH_ALEN) {                                                                \
                fprintf(stderr, "Failed to parse MAC Address!\n");                                \
                ret = -EINVAL;                                                                    \
                goto err_parse;                                                                   \
            }                                                                                     \
                                                                                                  \
            for (i = 0; i < ETH_ALEN; ++i)                                                        \
                app_config.var[i] = (unsigned char)tmp[i];                                        \
        }                                                                                         \
    } while (0)

#define CONFIG_STORE_CLOCKID_PARAM(name, var)                                \
    do {                                                                     \
        if (!strcasecmp(key, #name)) {                                       \
            if (!strcasecmp(value, "CLOCK_TAI"))                             \
                app_config.var = CLOCK_TAI;                                  \
            else if (!strcasecmp(value, "CLOCK_MONOTONIC"))                  \
                app_config.var = CLOCK_MONOTONIC;                            \
            else {                                                           \
                fprintf(stderr, "Invalid clockid specified! '%s'\n", value); \
                goto err_parse;                                              \
            }                                                                \
        }                                                                    \
    } while (0)

#define CONFIG_STORE_ETHER_TYPE(name, var)                                \
    do {                                                                  \
        if (!strcasecmp(key, #name)) {                                    \
            app_config.var = strtoul(value, &endptr, 16);                 \
            if (errno != 0 || endptr == value || *endptr != '\0') {       \
                ret = -ERANGE;                                            \
                fprintf(stderr, "The value for " #name " is invalid!\n"); \
                goto err_parse;                                           \
            }                                                             \
        }                                                                 \
    } while (0)

#define CONFIG_STORE_SECURITY_MODE_PARAM(name, var)                                                \
    do {                                                                                           \
        if (!strcasecmp(key, #name)) {                                                             \
            if (strcasecmp(value, "none") && strcasecmp(value, "ao") && strcasecmp(value, "ae")) { \
                fprintf(stderr, "Invalid security mode specified!\n");                             \
                goto err_parse;                                                                    \
            }                                                                                      \
                                                                                                   \
            if (!strcasecmp(value, "none"))                                                        \
                app_config.var = SECURITY_MODE_NONE;                                               \
            if (!strcasecmp(value, "ao"))                                                          \
                app_config.var = SECURITY_MODE_AO;                                                 \
            if (!strcasecmp(value, "ae"))                                                          \
                app_config.var = SECURITY_MODE_AE;                                                 \
        }                                                                                          \
    } while (0)

#define CONFIG_STORE_SECURITY_ALGORITHM_PARAM(name, var)                              \
    do {                                                                              \
        if (!strcasecmp(key, #name)) {                                                \
            if (strcasecmp(value, "aes256-gcm") && strcasecmp(value, "aes128-gcm") && \
                strcasecmp(value, "chacha20-poly1305")) {                             \
                fprintf(stderr, "Invalid security algorithm specified!\n");           \
                goto err_parse;                                                       \
            }                                                                         \
            if (!strcasecmp(value, "aes256-gcm"))                                     \
                app_config.var = SECURITY_ALGORITHM_AES256_GCM;                       \
            if (!strcasecmp(value, "aes128-gcm"))                                     \
                app_config.var = SECURITY_ALGORITHM_AES128_GCM;                       \
            if (!strcasecmp(value, "chacha20-poly1305"))                              \
                app_config.var = SECURITY_ALGORITHM_CHACHA20_POLY1305;                \
        }                                                                             \
    } while (0)

#define CONFIG_IS_TRAFFIC_CLASS_ACTIVE(name)                                         \
    ({                                                                               \
        bool __ret = false;                                                          \
        if (app_config.name##_enabled && app_config.name##_num_frames_per_cycle > 0) \
            __ret = true;                                                            \
        __ret;                                                                       \
    })

#define CONFIG_STORE_LPORT_PARAM(name, var)                                         \
    do {                                                                            \
        if (!strcasecmp(key, #name)) {                                              \
            uint16_t tmp[2];                                                        \
            int ret = sscanf(value, "%hu:%hu", &tmp[0], &tmp[1]);                   \
            if (ret != 2 || tmp[0] > 65535 || tmp[1] > 65535) {                     \
                fprintf(stderr, "%s: Invalid LPort %s specified!\n", #name, value); \
                ret = -EINVAL;                                                      \
                goto err_parse;                                                     \
            }                                                                       \
            app_config.var = lport_make(tmp[0], tmp[1]);                            \
        }                                                                           \
    } while (0)

static inline bool
config_have_busy_poll(void)
{
#if defined(HAVE_SO_BUSY_POLL) && defined(HAVE_SO_PREFER_BUSY_POLL) && \
    defined(HAVE_SO_BUSY_POLL_BUDGET)
    return true;
#else
    return false;
#endif
}

static inline bool
config_have_mosquitto(void)
{
#if defined(WITH_MQTT)
    return true;
#else
    return false;
#endif
}

#endif /* _CONFIG_H_ */

/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2025-2025 Intel Corporation
 */

#ifndef _FUNCTIONS_H_
#define _FUNCTIONS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum function_idx {
    NO_IDX = -1,             // No index
    CONFIG_IDX,              // Configuration
    LPORT_IDX,               // LPORT
    LOG_IDX,                 // Logging
    LOG_VIA_MQTT_IDX,        // Logging via MQTT
    STAT_IDX,                // Statistics
    HISTOGRAM_IDX,           // Histogram
    PKT_HANDLER_IDX,         // Packet handler
    L2_IDX,                  // Layer 2
    TSN_HIGH_IDX,            // TSN High Priority
    TSN_LOW_IDX,             // TSN Low Priority
    RTC_IDX,                 // Real-Time Clock
    RTA_IDX,                 // Real-Time Alarm
    DCP_IDX,                 // Data Control Protocol
    LLDP_IDX,                // Link Layer Discovery Protocol
    UDP_HIGH_IDX,            // UDP High Priority
    UDP_LOW_IDX,             // UDP Low Priority
    MAX_FUNCTIONS            // Number of supported functions
} function_idx_t;

#define MAX_PN_TYPES (MAX_FUNCTIONS - TSN_HIGH_IDX)        // maximum number of Profinet threads

typedef struct function_arg {
    const char *msg;                                // Message
    void *thread_context;                           // Thread context
    int (*init_fn)(struct thread_context *);        // Function initialize routine
} function_arg_t;

typedef struct function_s {
    bool valid;                               // Function is valid
    const char *name;                         // Function name
    int (*init_fn)(void *);                   // Function initialization
    int (*launch_fn)(void *);                 // Function launch
    void (*deinit_fn)(void *);                // Function deinitialize
    function_idx_t thread_idx;                // Function index
    struct thread_context *thread_ctx;        // Thread context
} function_t;

#define FUNCTION_REGISTER(_name, _idx)                                                   \
    static function_t _name##_funcs = {                                                  \
        .name       = #_name,                                                            \
        .init_fn    = _name##_init,                                                      \
        .launch_fn  = _name##_launch,                                                    \
        .deinit_fn  = _name##_deinit,                                                    \
        .thread_idx = _idx,                                                              \
    };                                                                                   \
    RTE_INIT_PRIO(_name##_constructor, LAST)                                             \
    {                                                                                    \
        if (function_register(&_name##_funcs))                                           \
            rte_exit(EXIT_FAILURE, "Cannot register %s function\n", _name##_funcs.name); \
    }

int function_register(function_t *funcs);

int function_init_all(void);
int function_launch_all(void);
void function_stop_all(void);
void function_free_all(void);

int function_link_pn_threads(void);

void function_list(int idx);
void function_list_all(void);

#ifdef __cplusplus
}
#endif

#endif /* _FUNCTIONS_H_ */

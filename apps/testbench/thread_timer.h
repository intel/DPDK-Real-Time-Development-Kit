/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2025-2025 Intel Corporation
 */

#ifndef _THREAD_TIMER_H_
#define _THREAD_TIMER_H_

#include <bsd/sys/queue.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rte_atomic.h>
#include <rte_cycles.h>

#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RX_TIMER = 0,
    TX_TIMER,
    TXGEN_TIMER,
    MAX_TIMERS,
} timer_id_t;

#define TIMER_ID_STRING {"Rx:", "Tx:", "TxGen:"}

enum {
    TIMER_NAME_LENGTH  = 32,
    MAX_DEFAULT_TIMERS = 4,
    WAIT_TIMEOUT_SEC   = 10,
    TIMER_MIN_DEFAULT  = 1000000UL,
};
struct thread_timer_info;
struct thread_context;

typedef void (*timer_callback_t)(void *data, bool signaled);

typedef struct avg_s {
    uint64_t sum;
    uint64_t cnt;
} timer_avg_t;

struct stat_avg;

typedef struct thread_timer_info {
    char name[TIMER_NAME_LENGTH];        // Timer name
    uint16_t used;                       // Timer used flag
    uint16_t active;                     // Timer active flag
    rte_atomic16_t *cond;                // Condition variable for synchronization
    timer_callback_t fn;                 // Timer callback function
    void *arg;                           // Timer callback argument
    volatile uint64_t timo_ns;           // Timer duration in ns
    volatile uint64_t end_ns;            // Timer end time in ns
    timer_avg_t avg;                     // Avg for end_ns compared to clock time
} thread_timer_info_t __rte_cache_aligned;

typedef struct thread_timer {
    TAILQ_ENTRY(thread_timer) next;        // List of next hmap entries
    char name[TIMER_NAME_LENGTH];          // Thread Timer name
    thread_timer_info_t *info;             // Thread Timer info array
    uint16_t nb_timers;                    // Number of timers
    uint16_t stop;                         // Flag to stop the timer loop
    uint16_t stopped;                      // Flag to indict timer loop stopped
} thread_timer_t;

int thread_timer_alloc(struct thread_context *ctx, const char *name, uint8_t nb_timers);
int thread_timer_add(struct thread_context *ctx, timer_id_t id, const char *name,
                     rte_atomic16_t *cond, timer_callback_t f, void *arg, uint64_t curr_ns,
                     uint64_t timo_ns);
void thread_timer_remove(struct thread_context *ctx, timer_id_t id);
void thread_timer_free(struct thread_context *ctx);
int thread_timer_run(struct thread_context *ctx);
void thread_timer_stop_all(void);
void thread_timer_set(struct thread_context *ctx, timer_id_t id, uint64_t timo_ns);
uint64_t thread_timer_end_ns(struct thread_context *ctx, timer_id_t id);
struct stat_avg *thread_timer_avg(struct thread_context *ctx, struct stat_avg *avg);

#define MIN_WAIT_TIME     16
#define WAIT_TIME_MASK    (~(MIN_WAIT_TIME - 1))

static inline void
wait_sync_time(uint32_t requested)
{
    uint32_t sec = requested;

    if (sec > 0) {
        struct timespec wakeup_time;
        int ret;

        sec = RTE_ALIGN(sec, MIN_WAIT_TIME);
        clock_gettime(CLOCK_TAI, &wakeup_time);

        wakeup_time.tv_sec &= WAIT_TIME_MASK;
        wakeup_time.tv_sec += sec;
        wakeup_time.tv_nsec = 0;

        do {
            ret = clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &wakeup_time, NULL);
        } while (ret == EINTR);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* _THREAD_TIMER_H_ */

/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2025-2025 Intel Corporation
 */

#include <bsd/sys/queue.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include "config.h"
#include "log.h"
#include "thread.h"
#include "thread_timer.h"

static TAILQ_HEAD(thread_timer_list, thread_timer) timer_list = TAILQ_HEAD_INITIALIZER(timer_list);
static pthread_mutex_t timer_list_mutex;

#define ERR_RET(code, ...)                        \
    do {                                          \
        fprintf(stderr, "%s: error: ", __func__); \
        fprintf(stderr, __VA_ARGS__);             \
        return code;                              \
    } while (0)

static inline void
timer_list_lock(void)
{
    int ret = pthread_mutex_lock(&timer_list_mutex);

    if (ret)
        fprintf(stderr, "failed: %s\n", strerror(ret));
}

static inline void
timer_list_unlock(void)
{
    int ret = pthread_mutex_unlock(&timer_list_mutex);

    if (ret)
        fprintf(stderr, "failed: %s\n", strerror(ret));
}

static void
thread_timer_quit(thread_timer_t *timer)
{
    uint64_t cur_tsc = clock_gettime_ns();
    uint64_t timo    = cur_tsc + (NSEC_PER_SEC * 2);        // Wait 2 seconds for stopping

    timer->stop = 1;

    while (timer->stopped == 0) {
        cur_tsc = clock_gettime_ns();
        if (cur_tsc >= timo) {
            fprintf(stderr, "%s:  timer %s timed out\n", __func__, timer->name);
            return;
        }
        rte_pause();
    }
}

int
thread_timer_alloc(struct thread_context *ctx, const char *name, uint8_t nb_timers)
{
    thread_timer_t *timer;

    // allocate the thread timer structure and zero all fields
    timer = rte_zmalloc_socket(name, sizeof(thread_timer_t), 0, rte_socket_id());
    if (!timer)
        ERR_RET(-1, "Failed to allocate thread timer\n");

    strlcpy(timer->name, name, sizeof(timer->name));

    if (nb_timers == 0)
        nb_timers = MAX_DEFAULT_TIMERS;

    timer->nb_timers = nb_timers;

    // initialize the timer info array and zero all timer info fields
    timer->info =
        rte_zmalloc_socket(name, sizeof(thread_timer_info_t) * nb_timers, 0, rte_socket_id());
    if (!timer->info)
        ERR_RET(-1, "Failed to allocate thread timer info\n");

    timer_list_lock();
    TAILQ_INSERT_TAIL(&timer_list, timer, next);
    timer_list_unlock();

    ctx->timer = timer;

    return 0;
}

void
thread_timer_free(struct thread_context *ctx)
{
    thread_timer_t *timer;

    if (!ctx || ctx->timer == NULL)
        return;
    timer      = ctx->timer;
    ctx->timer = NULL;

    thread_timer_quit(timer);

    // clear the timer active flags
    for (uint16_t idx = 0; idx < timer->nb_timers; idx++)
        timer->info[idx].active = 0;

    rte_free(timer->info);
    rte_free(timer);
}

int
thread_timer_add(struct thread_context *ctx, timer_id_t id, const char *name, rte_atomic16_t *cond,
                 timer_callback_t f, void *arg, uint64_t curr_ns, uint64_t timo_ns)
{
    thread_timer_t *timer;
    thread_timer_info_t *info;

    if (!ctx || ctx->timer == NULL)
        ERR_RET(-1, "Invalid thread context\n");
    timer = ctx->timer;
    info  = &timer->info[id];

    if (info->used == 0) {
        info->used = 1;
        strlcpy(info->name, name, sizeof(info->name));

        info->cond = cond;
        info->fn   = f;
        info->arg  = arg;

        info->timo_ns = timo_ns;
        info->end_ns  = curr_ns + timo_ns;

        info->active = 1;
        return 0;
    }
    ERR_RET(-1, "timer ID %d is already used\n", id);
}

void
thread_timer_set(struct thread_context *ctx, timer_id_t id, uint64_t timo_ns)
{
    thread_timer_info_t *info = &ctx->timer->info[id];

    info->timo_ns = timo_ns;
}

uint64_t
thread_timer_end_ns(struct thread_context *ctx, timer_id_t id)
{
    if (!ctx)
        ERR_RET(0, "Invalid thread context\n");

    if (id >= ctx->timer->nb_timers)
        ERR_RET(0, "Invalid timer ID\n");
    return ctx->timer->info[id].end_ns;
}

struct stat_avg *
thread_timer_avg(struct thread_context *ctx, struct stat_avg *s)
{
    thread_timer_t *timer = ctx->timer;
    timer_avg_t *avg;

    avg   = &timer->info[RX_TIMER].avg;
    s->rx = avg->sum / (double)((avg->cnt == 0) ? 1 : avg->cnt);

    avg   = &timer->info[TX_TIMER].avg;
    s->tx = avg->sum / (double)((avg->cnt == 0) ? 1 : avg->cnt);

    avg      = &timer->info[TXGEN_TIMER].avg;
    s->txgen = avg->sum / (double)((avg->cnt == 0) ? 1 : avg->cnt);

    return s;
}

int
thread_timer_run(struct thread_context *ctx)
{
    thread_timer_t *timer;

    if (!ctx)
        ERR_RET(-1, "Invalid thread context\n");

    timer = ctx->timer;
    if (!timer)
        ERR_RET(-1, "Invalid thread timer\n");

    wait_sync_time(app_config.application_wait_time_before_start);

    for (uint16_t idx = 0; idx < timer->nb_timers; idx++) {
        thread_timer_info_t *info = &timer->info[idx];

        info->end_ns = clock_gettime_ns() + info->timo_ns;
    }

    while (!timer->stop) {
        bool signaled = false;

        for (uint16_t idx = 0; idx < timer->nb_timers; idx++) {
            thread_timer_info_t *info = &timer->info[idx];

            if (!info->active)
                continue;

            // Check if we were signaled or expired, if enabled
            signaled = (info->cond && is_signaled(info->cond));

            uint64_t curr_ns = clock_gettime_ns();

            if ((curr_ns >= info->end_ns) || signaled) {
                info->fn(info->arg, signaled);        // Call the timer function
                info->end_ns += info->timo_ns;        // increment by the cycle time
            }
        }
    }
    timer->stopped = 1;

    return 0;
}

void
thread_timer_stop_all(void)
{
    thread_timer_t *timer, *tvar;

    timer_list_lock();
    TAILQ_FOREACH_SAFE (timer, &timer_list, next, tvar) {
        thread_timer_quit(timer);
    }
    timer_list_unlock();
}

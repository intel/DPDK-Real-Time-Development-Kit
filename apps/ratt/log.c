/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/*
 * This application is a simple reference and mirror application to measure the
 * performance sending a fixed set of packets at a given cycle time.
 */
#include <rte_hexdump.h>
#include "log.h"

typedef struct log_info {
    struct rte_ring *ring;              // Log ring for statistics
    struct rte_mempool *pool;           // Memory pool for log ring
    struct rte_ring *delta_ring;        // Log ring for delta values one uint64_t per entry
    FILE *fd;                           // Log file descriptor
    char *file;                         // Log file name
} log_info_t;

static log_info_t log_info = {0};
static log_info_t *linfo   = &log_info;

int
log_init(char *logfile)
{
    int socket_id = rte_socket_id();

    linfo->ring =
        rte_ring_create("log", LOG_RING_SIZE, socket_id, RING_F_MC_RTS_DEQ | RING_F_MP_RTS_ENQ);
    if (!linfo->ring) {
        printf("Error: Could not create log ring\n");
        return -ENOMEM;
    }

    linfo->pool = rte_mempool_create("log_pool", LOG_BUFFER_POOL_COUNT, LOG_BUFFER_SIZE, 32,
                                     DEFAULT_PRIV_SIZE, NULL, NULL, NULL, NULL, socket_id, 0);
    if (!linfo->pool) {
        printf("Error: Could not create log pool\n");
        return -ENOMEM;
    }

    uint32_t ring_size = pow2roundup(((NS_PER_S / pinfo->cycle_time_ns) * 2));
    linfo->delta_ring =
        rte_ring_create("delta", ring_size, socket_id, RING_F_MC_RTS_DEQ | RING_F_MP_RTS_ENQ);
    if (!linfo->delta_ring) {
        printf("Error: Could not create delta ring\n");
        return -ENOMEM;
    }

    linfo->file = strdup(logfile);
    return log_open();
}

void
log_close(void)
{
    if (pinfo->log_enabled) {
        log_flush();
        rte_ring_free(linfo->ring);        // These check for NULL pointers
        rte_ring_free(linfo->delta_ring);
        rte_mempool_free(linfo->pool);

        if (linfo->file)
            free(linfo->file);
        if (linfo->fd)
            fclose(linfo->fd);

        memset(linfo, 0, sizeof(log_info_t));
    }
}

int
log_open(void)
{
    if (linfo->fd)
        fclose(linfo->fd);

    if (!linfo->file)
        return 0;

    unlink(linfo->file);

    linfo->fd = fopen(linfo->file, "w");
    if (!linfo->fd) {
        printf("Error: Failed to open log file '%s'\n", linfo->file);
        return -1;
    }
    return 0;
}

void
log_message(const char *format, ...)
{
    int written, len;
    struct timespec time = {0};
    va_list args;
    char *p, *buff;

    if (!pinfo->log_enabled)
        return;

    if (rte_mempool_get(linfo->pool, (void **)&buff))
        return;

    /* Log each message with timestamp. */
    clock_gettime(CLOCK_TAI, &time);

    len  = LOG_BUFFER_SIZE - 1;
    p    = buff;
    p[0] = '\0';

    written = snprintf(p, len, "[%8ld.%9ld]: ", time.tv_sec, time.tv_nsec);
    p += written;
    len -= written;

    va_start(args, format);
    written += vsnprintf(p, len, format, args);
    va_end(args);

    rte_ring_enqueue(linfo->ring, p);
}

void
log_delta(uint64_t delta)
{
    if (!pinfo->log_enabled)
        return;
    if (!pinfo->deltas_enabled)
        return;

    if (rte_ring_enqueue(linfo->delta_ring, (void *)delta))
        fprintf(stderr, "Failed to enqueue log delta statistic to ring\n");
}

static void
flush_logs(void)
{
    void *data[LOG_BURST_COUNT];
    int nb_buffs;

    if (!pinfo->log_enabled || linfo->ring == NULL)
        return;

    while (rte_ring_count(linfo->ring) > 0) {
        nb_buffs = rte_ring_dequeue_burst(linfo->ring, data, LOG_BURST_COUNT, NULL);
        if (nb_buffs) {
            for (int i = 0; i < nb_buffs; i++)
                fprintf(linfo->fd, "%s\n", (char *)data[i]);

            rte_mempool_put_bulk(linfo->pool, data, nb_buffs);
        }
    }
}

static void
flush_deltas(void)
{
    uint64_t data[LOG_BURST_COUNT];

    if (!pinfo->log_enabled)
        return;
    if (!pinfo->deltas_enabled || linfo->delta_ring == NULL)
        return;

    while (rte_ring_count(linfo->delta_ring) > 0) {
        // The rte_ring_count() test above means we have entries in the ring.
        int nb_deltas =
            rte_ring_dequeue_burst(linfo->delta_ring, (void **)data, LOG_BURST_COUNT, NULL);

        if (nb_deltas > 0)
            fprintf(linfo->fd, "Deltas(%d):\n", nb_deltas);
        for (int i = 0; i < nb_deltas; i++)
            fprintf(linfo->fd, "\t%" PRIu64 "\n", data[i]);
    }
}

void
log_flush(void)
{
    flush_logs();
    flush_deltas();
    fflush(linfo->fd);
}

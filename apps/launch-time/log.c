/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <unistd.h>
#include "log.h"

typedef struct log_info {
    struct rte_ring *ring;              // Log ring for statistics
    struct rte_mempool *pool;           // Memory pool for log ring
    FILE *fd;                           // Log file descriptor
    char *file;                         // Log file name
} log_info_t;

static log_info_t log_info = {0};
static log_info_t *linfo   = &log_info;

int
log_init(const char *logfile)
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
        goto err_pool;
    }

    linfo->file = strdup(logfile);
    if (!linfo->file)
        goto err_file;

    return log_open();

err_file:
    rte_mempool_free(linfo->pool);
    linfo->pool = NULL;
err_pool:
    rte_ring_free(linfo->ring);
    linfo->ring = NULL;
    return -ENOMEM;
}

void
log_close(void)
{
    if (_btst(LOG)) {
        _bclr(LOG);
        log_flush();
        rte_ring_free(linfo->ring);        // These check for NULL pointers
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

    if (!_btst(LOG))
        return;

    if (rte_mempool_get(linfo->pool, (void **)&buff))
        return;

    /* Log each message with timestamp. */
    if (clock_gettime(CLOCK_TAI, &time) < 0)
        memset(&time, 0, sizeof(time));

    len  = LOG_BUFFER_SIZE - 1;
    p    = buff;
    p[0] = '\0';

    written = snprintf(p, len, "[%8ld.%9ld]: ", time.tv_sec, time.tv_nsec);
    if (written > 0 && written < len) {
        p += written;
        len -= written;
    }

    va_start(args, format);
    vsnprintf(p, len, format, args);
    va_end(args);

    if (rte_ring_enqueue(linfo->ring, buff))
        rte_mempool_put(linfo->pool, buff);
}

static void
flush_logs(void)
{
    void *data[LOG_BURST_COUNT];
    int nb_buffs;

    if (!_btst(LOG) || linfo->ring == NULL)
        return;

    while (rte_ring_count(linfo->ring) > 0) {
        nb_buffs = rte_ring_dequeue_burst(linfo->ring, data, LOG_BURST_COUNT, NULL);
        if (nb_buffs) {
            for (int i = 0; i < nb_buffs; i++) {
                if (linfo->fd)
                    fprintf(linfo->fd, "%s\n", (char *)data[i]);
            }

            rte_mempool_put_bulk(linfo->pool, data, nb_buffs);
        }
    }
}

void
log_flush(void)
{
    flush_logs();
    if (linfo->fd)
        fflush(linfo->fd);
}

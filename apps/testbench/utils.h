/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _UTILS_H_
#define _UTILS_H_

#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rte_cycles.h>
#include <rte_atomic.h>
#include <rte_time.h>

#include "net_def.h"
#include "security.h"
#include "stat.h"
#include "lport.h"
#include "lport-priv.h"

static inline uint64_t
clock_gettime_ns(void)
{
    struct timespec ctime = {0};

    if (clock_gettime(app_config.application_clock_id, &ctime) < 0)
        return 0;
    return rte_timespec_to_ns(&ctime);
}

void init_mutex(pthread_mutex_t *mutex);
void init_condition_variable(pthread_cond_t *cond_var);

void increment_period(struct timespec *time, int64_t period_ns);

void swap_mac_addresses(void *buffer, size_t len);
void insert_vlan_tag(void *buffer, size_t len, uint16_t vlan_tci);

/*
 * This function takes an received Ethernet frame by AF_PACKET sockets and performs two tasks:
 *
 *  1.) Inject VLAN header
 *  2.) Swap source and destination
 *
 * This function does nothing when the @newFrame isn't sufficient in length.
 */
void build_vlan_frame_from_rx(const unsigned char *old_frame, size_t old_frame_len,
                              unsigned char *new_frame, size_t new_frame_len, uint16_t ether_type,
                              uint16_t vlan_tci);

/*
 * This function initializes an PROFINET Ethernet frame. The Ethernet header, PROFINET header and
 * payload is initialized. The sequenceCounter is set to zero.
 *
 * In case the SecurityMode is AE or AO, the PROFINET Ethernet frames will contain the
 * SecurityHeader after the FrameID.
 */
void initialize_profinet_frame(enum security_mode mode, unsigned char *frame_data,
                               size_t frame_length, const unsigned char *source,
                               const unsigned char *destination, const char *payload_pattern,
                               size_t payload_pattern_length, uint16_t vlan_tci, uint16_t frame_id);

/*
 * The following function prepares an already initialized PROFINET Ethernet frame for final
 * transmission. Depending on traffic class and security modes, different actions have to be taken
 * e.g., adjusting the cycle counter and perform authentication and/or encryption.
 */

struct prepare_frame_config {
    enum security_mode mode;
    struct security_context *security_context;
    const unsigned char *iv_prefix;
    const unsigned char *payload_pattern;
    size_t payload_pattern_length;
    unsigned char *frame_data;
    size_t frame_length;
    size_t num_frames_per_cycle;
    uint64_t sequence_counter;
    uint64_t tx_timestamp;
    uint32_t meta_data_offset;
};

int prepare_frame_for_tx(const struct prepare_frame_config *frame_config);

void prepare_iv(const unsigned char *iv_prefix, uint64_t sequence_counter, struct security_iv *iv);

void prepare_openssl(struct security_context *context);

void configure_cpu_latency(void);
void restore_cpu_latency(void);

/* error handling */
void pthread_error(int ret, const char *message);

/* Printing */
void print_mac_address(const unsigned char *mac_address);
void print_payload_pattern(const char *payload_pattern, size_t payload_pattern_length);

void hexdump(FILE *f, const char *title, const void *buf, unsigned int len);
void memdump(FILE *f, const char *title, const void *buf, unsigned int len);

#define ARRAY_SIZE(x)   (sizeof(x) / sizeof((x)[0]))
#define PTR_ADD(ptr, x) ((void *)((uintptr_t)(ptr) + (x)))

#define BIT(x) (1ULL << (x))

/* Meta data handling */
static inline uint64_t
meta_data_to_sequence_counter(const struct reference_meta_data *meta, size_t num_frames_per_cycle)
{
    uint32_t frame_counter, cycle_counter;

    frame_counter = be32toh(meta->frame_counter);
    cycle_counter = be32toh(meta->cycle_counter);

    return (uint64_t)cycle_counter * num_frames_per_cycle + (uint64_t)frame_counter;
}

static inline void
sequence_counter_to_meta_data(struct reference_meta_data *meta, uint64_t sequence_counter,
                              size_t num_frames_per_cycle)
{
    unsigned int counter       = sequence_counter % num_frames_per_cycle;
    unsigned int cycle_counter = sequence_counter / num_frames_per_cycle;

    meta->frame_counter = htobe32(counter);
    meta->cycle_counter = htobe32(cycle_counter);
}

static inline void
tx_timestamp_to_meta_data(struct reference_meta_data *const meta, uint64_t timestamp)
{
    timestamp = htobe64(timestamp);
    memcpy(&meta->tx_timestamp, &timestamp, sizeof(meta->tx_timestamp));
}

static inline uint64_t
meta_data_to_tx_timestamp(const struct reference_meta_data *meta)
{
    uint64_t tx_timestamp;

    tx_timestamp = be64toh(meta->tx_timestamp);

    return tx_timestamp;
}

static inline uint64_t
get_sequence_counter(const unsigned char *frame_data, uint32_t meta_data_offset,
                     size_t num_frames_per_cycle)
{
    struct reference_meta_data const *meta_data;

    meta_data = (struct reference_meta_data const *)(frame_data + meta_data_offset);

    return meta_data_to_sequence_counter(meta_data, num_frames_per_cycle);
}

static inline void
set_sequence_counter(unsigned char *frame_data, uint32_t meta_data_offset,
                     uint64_t sequence_counter, size_t num_frames_per_cycle)
{
    struct reference_meta_data *meta_data;

    meta_data = (struct reference_meta_data *)(frame_data + meta_data_offset);

    sequence_counter_to_meta_data(meta_data, sequence_counter, num_frames_per_cycle);
}

uint32_t get_meta_data_offset(enum stat_frame_type frame_type, enum security_mode security_mode);
int send_frames_common(enum stat_frame_type frame_type, lport_id_t id, uint32_t meta_data_offset,
                       size_t frames_per_cycle, uint64_t timestamp);

static inline int
is_signaled(rte_atomic16_t *var)
{
    // if set then we have been signaled
    return rte_atomic16_cmpset((volatile uint16_t *)&var->cnt, 1, 0) != 0;
}

static inline void
do_signal(rte_atomic16_t *var)
{
    // if already set then return as it is currently signaled
    rte_atomic16_cmpset((volatile uint16_t *)&var->cnt, 0, 1);
}

static inline int
do_once(rte_atomic16_t *var)
{
    return rte_atomic16_test_and_set(var) > 0;
}

static inline void
atomic16_set(rte_atomic16_t *var, uint16_t value)
{
    rte_atomic16_set(var, value);
}

static inline uint16_t
atomic16_read(rte_atomic16_t *var)
{
    return rte_atomic16_read(var);
}

static inline uint16_t
atomic16_test_and_set(rte_atomic16_t *var)
{
    return rte_atomic16_test_and_set(var) > 0;
}

static inline uint64_t
atomic64_exchange(rte_atomic64_t *var, uint64_t value)
{
    return rte_atomic64_exchange((volatile uint64_t *)&var->cnt, value);
}

static inline void
atomic64_set(rte_atomic64_t *var, uint64_t value)
{
    rte_atomic64_set(var, value);
}

static inline void
atomic64_add(rte_atomic64_t *var, uint64_t value)
{
    rte_atomic64_add(var, value);
}

static inline void
atomic64_inc(rte_atomic64_t *var)
{
    rte_atomic64_inc(var);
}

/**
 * Helper routine to create a mutex with a specific type.
 *
 * @param mutex
 *   The pointer to the mutex to create.
 * @param flags
 *   The attribute flags used to create the mutex i.e. recursive attribute
 * @return
 *   0 on success or -1 on failure errno is set
 */
static inline int
mutex_create(pthread_mutex_t *mutex, int flags)
{
    pthread_mutexattr_t attr;
    int inited = 0, ret = EFAULT;

    if (!mutex)
        goto err;

#define __do(_exp)    \
    do {              \
        ret = _exp;   \
        if (ret)      \
            goto err; \
    } while (0 /* CONSTCOND */)

    __do(pthread_mutexattr_init(&attr));
    inited = 1;

    __do(pthread_mutexattr_settype(&attr, flags));

    __do(pthread_mutex_init(mutex, &attr));

    __do(pthread_mutexattr_destroy(&attr));

#undef __do
    return 0;
err:
    if (inited) {
        /* Do not lose the previous error value */
        if (pthread_mutexattr_destroy(&attr))
            fprintf(stderr, "Failed to destroy mutex attribute, but is not the root cause\n");
    }

    errno = ret;
    return -1;
}

/**
 * Destroy a mutex
 *
 * @param mutex
 *   Pointer to mutex to destroy.
 * @return
 *   0 on success and -1 on error with errno set.
 */
static inline int
mutex_destroy(pthread_mutex_t *mutex)
{
    int ret = 0;

    if (mutex)
        ret = pthread_mutex_destroy(mutex);

    errno = ret;
    return (ret != 0) ? -1 : 0;
}
#endif /* _UTILS_H_ */

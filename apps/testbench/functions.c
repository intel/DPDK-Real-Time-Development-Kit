/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2025-2025 Intel Corporation
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include "config.h"
#include "functions.h"
#include "log.h"
#include "thread.h"
#include "utils.h"

static function_t funcs[MAX_FUNCTIONS] = {0};
static struct thread_context threads[MAX_FUNCTIONS] = {0};

int function_register(function_t *func)
{
	if (!func)
		rte_exit(EXIT_FAILURE, "Invalid function pointer\n");

	int idx = func->thread_idx;

	if (idx >= MAX_FUNCTIONS)
		rte_exit(EXIT_FAILURE, "Too many functions registered\n");

	if (funcs[idx].valid)
		rte_exit(EXIT_FAILURE, "Function '%s' (%d) already registered\n", func->name,
			func->thread_idx);

	funcs[idx] = *func;
	funcs[idx].valid = true;
	funcs[idx].thread_ctx = &threads[idx];
	funcs[idx].thread_ctx->name = func->name;

	return 0;
}

int function_init_all(void)
{
	for (int i = 0; i < MAX_FUNCTIONS; i++) {
		function_t *fn = &funcs[i];

		if (!fn->valid || !fn->init_fn)
			continue;

		if (fn->init_fn(fn->thread_ctx))
			rte_exit(EXIT_FAILURE, "@@@ Error initializing Function index %3d: '%s'\n",
                                 fn->thread_idx, fn->name);
	}
	return 0;
}

int function_launch_all(void)
{
	for (int i = 0; i < MAX_FUNCTIONS; i++) {
		function_t *fn = &funcs[i];

		if (!fn->valid || !fn->launch_fn)
			continue;

		if (fn->launch_fn(fn->thread_ctx))
			rte_exit(EXIT_FAILURE, "@@@ Error launching Function index %3d: '%s'\n",
				 fn->thread_idx, fn->name);
	}
	return 0;
}

void function_stop_all(void)
{
	for (int i = 0; i < MAX_FUNCTIONS; i++) {
		function_t *fn = &funcs[i];

		if (fn->valid)
			fn->thread_ctx->stop = true;
	}
}

void function_free_all(void)
{
	for (int i = MAX_FUNCTIONS - 1; i >= 0; i--) {
		function_t *fn = &funcs[i];

		if (fn->valid && fn->deinit_fn)
			fn->deinit_fn(fn->thread_ctx);
	}
}

static struct thread_context *find_next_pn_thread(int start)
{
	switch (start) {
	case TSN_HIGH_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_low))
			return &threads[TSN_LOW_IDX];
		/* FALLTHRU */
	case TSN_LOW_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rtc))
			return &threads[RTC_IDX];
		/* FALLTHRU */
	case RTC_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rta))
			return &threads[RTA_IDX];
		/* FALLTHRU */
	case RTA_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(dcp))
			return &threads[DCP_IDX];
		/* FALLTHRU */
	case DCP_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(lldp))
			return &threads[LLDP_IDX];
		/* FALLTHRU */
	case LLDP_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_high))
			return &threads[UDP_HIGH_IDX];
		/* FALLTHRU */
	case UDP_HIGH_IDX:
		if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(udp_low))
			return &threads[UDP_LOW_IDX];
		/* FALLTHRU */
	case UDP_LOW_IDX:
		return NULL;
	}

	return NULL;
}

int function_link_pn_threads(void)
{
	/*
	 * The Profinet traffic classes have a dedicated order:
	 *   TSN High -> TSN Low -> RTC -> RTA -> DCP -> LLDP -> UDP High -> UDP Low
	 *
	 * This code will link the traffic classes in order. Non-active traffic classes will be
	 * skipped.
	 */
	for (int i = 0; i < MAX_PN_TYPES; i++)
		threads[TSN_HIGH_IDX + i].next = find_next_pn_thread(TSN_HIGH_IDX + i);

	/*
	 * The first traffic class is either
	 *  a) TSN in case of Profinet TSN, or
	 *  b) RTC in case of Profinet RT.
	 *
	 * L2 has nothing todo with Profinet.
	 */
	if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(tsn_high)) {
		threads[TSN_HIGH_IDX].is_first = true;
	} else if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(rtc)) {
		threads[RTC_IDX].is_first = true;
	} else if (CONFIG_IS_TRAFFIC_CLASS_ACTIVE(l2)) {
		return 0;
	} else
		return -EINVAL;
	return 0;
}

void function_list(int idx)
{
	function_t *func = &funcs[idx];

	printf("Function %d: %s (%s)(%d)\n", func->thread_idx, func->name,
	       func->valid ? "valid" : "invalid", func->valid);
	printf("\tinit  : %p\n", func->init_fn);
	printf("\tdeinit: %p\n", func->deinit_fn);
	printf("\tlaunch: %p\n", func->launch_fn);
}

void function_list_all(void)
{
	for (int i = 0; i < MAX_FUNCTIONS; i++)
		if (funcs[i].valid)
			function_list(i);
}

RTE_INIT_PRIO(function_constructor, CLASS)
{
}

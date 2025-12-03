/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2024 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* 1 MiB per traffic class */
#define LOG_RING_SIZE (8 * 1024)
#define LOG_MEMPOOL_BUFFER_COUNT (16 * 1024)
#define LOG_MEMPOOL_BUFFER_SIZE (6 * 1024)

enum log_level {
	LOG_LEVEL_ERROR = 1,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_INFO,
	LOG_LEVEL_DEBUG
};

void log_message(enum log_level level, const char *format, ...)
	__attribute__((__format__(printf, 2, 3)));

#endif // _LOG_H_

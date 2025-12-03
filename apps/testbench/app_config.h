/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020-2023 Linutronix GmbH
 * Author Kurt Kanzenbach <kurt@linutronix.de>
 */

#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

#include <stdbool.h>

#define VERSION "5.1"
#define INSTALL_EBPF_DIR "/usr/local/share/testbench/ebpf"

/* #undef WITH_MQTT */
#define HAVE_SO_BUSY_POLL 1
#define HAVE_SO_PREFER_BUSY_POLL 1
#define HAVE_SO_BUSY_POLL_BUDGET 1

extern bool mirror_mode;

static inline bool
is_mirror_mode(void)
{
	return mirror_mode;
}

#endif /* _APP_CONFIG_H_ */

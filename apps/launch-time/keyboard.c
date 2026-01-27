/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <unistd.h>
#include "launch-time.h"
#include "log.h"
#include "mqtt.h"

static int
poll_keyboard(uint8_t *c)
{
    struct pollfd fds;

    fds.fd      = 0;
    fds.events  = POLLIN;
    fds.revents = 0;

    if (poll(&fds, 1, 0)) {
        if ((fds.revents & (POLLERR | POLLNVAL)) == 0) {
            if ((fds.revents & POLLHUP))
                return -1;
            else if ((fds.revents & POLLIN)) {
                int n = read(fds.fd, c, 1);
                if (n > 0)
                    return 1;
            }
        }
    }
    return 0;
}

static int
stdin_setup(void)
{
    struct termios term;

    _bclr(TTY_IS_INITED);
    if (tcgetattr(0, &pinfo->oldterm)) {
        fprintf(stderr, "%s: failed to get tty\n", __func__);
        return -1;
    }

    memcpy(&term, &pinfo->oldterm, sizeof(term));

    term.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);

    if (tcsetattr(0, TCSANOW, &term)) {
        fprintf(stderr, "%s: failed to set tty\n", __func__);
        return -1;
    }
    _bset(TTY_IS_INITED);

    return 0;
}

void
keyboard_loop(void)
{
    uint32_t count = 0;
    uint8_t c;

    _bset(CLEAR_SCREEN);

    if (clock_gettime(CLOCK_TAI, &pinfo->start_time) < 0)
        memset(&pinfo->start_time, 0, sizeof(pinfo->start_time));

    stdin_setup();

    if (pinfo->run_duration_sec > 0)
        pinfo->run_duration_end_ns = clock_get_ns() + (pinfo->run_duration_sec * NSEC_PER_SEC);

    _bset(RESET_STATS);
    while (is_running()) {
        if ((count++ % 4) == 0)
            print_stats();
        log_flush();

        if (poll_keyboard(&c)) {
            switch (c) {
            case 'q':
                stop_running();
                break;
            case 'r':
                _bset(RESET_STATS);
                log_open();
                /* FALL-THRU */
            case 'c':
                _bset(CLEAR_SCREEN);
                break;
            default:
                fprintf(stderr, "Unknown command: %c\n", c);
                break;
            }
        }
        sleep_msec(250);
    }

    print_stats();        // update the stats one last time
    printf("Quitting ...\n");
    stop_running();
    sleep_msec(250);
    log_close();
    mqtt_close();
    stdin_restore();
}

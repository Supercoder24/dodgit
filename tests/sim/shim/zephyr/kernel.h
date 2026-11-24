/*
 * Host simulator shim for <zephyr/kernel.h> (tests/sim). Only what the product sources use.
 * Every function here is implemented by the simulator (tests/sim/src/sim_core.c); time is VIRTUAL:
 * a call only advances the calling node's own clock (plus a small modelled CPU cost).
 */
#ifndef SIM_ZEPHYR_KERNEL_H
#define SIM_ZEPHYR_KERNEL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* Milliseconds since this node booted (the Zephyr tick clock: 32768 Hz, floored to ms). */
int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);

/* Sleep, in Zephyr semantics: rounded UP to whole ticks (30.5 us), plus one tick of alignment. */
int32_t k_msleep(int32_t ms);
int32_t k_usleep(int32_t us);

/* Console output. On the real board this is a synchronous UART at 115200 baud, and the simulator
 * charges the caller the time that takes (see sim_set_console_baud). */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list ap);

#endif

/*
 * dodgit - ball debug log over Bluetooth (Nordic UART Service).
 *
 * This file implements dodgit_log() (shared/log.h; use it through LOG_CAT) for the ball: it always
 * prints the line to the console. If a laptop is connected and has subscribed (tools/ball_monitor.py)
 * the line is also queued for sending. Logging never blocks and never affects ranging or the state
 * machine: if the queue is full or nobody is connected the Bluetooth copy is simply dropped (and
 * counted, see ble_log_dropped()).
 */
#ifndef DODGIT_BLE_LOG_H
#define DODGIT_BLE_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Start advertising as "dodgit-ball". Returns 0 or -errno. */
int ble_log_init(void);

/* dodgit_log(): printf-style; a newline is appended. Lines are truncated at ~159 characters. */
#include "log.h"

bool ble_log_connected(void);   /* a laptop is subscribed */
uint32_t ble_log_dropped(void); /* lines lost because of a full queue or a dropped link */

#endif

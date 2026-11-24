/* Host simulator shim for <zephyr/random/random.h>: a deterministic per-node random stream. */
#ifndef SIM_ZEPHYR_RANDOM_H
#define SIM_ZEPHYR_RANDOM_H

#include <stdint.h>

uint32_t sys_rand32_get(void);

#endif

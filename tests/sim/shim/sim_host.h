/*
 * Host functions that the simulator's MOCKS (tests/sim/mocks/) call; implemented in
 * tests/sim/src/sim_core.c. The product sources never see this header.
 */
#ifndef SIM_HOST_H
#define SIM_HOST_H

#include <stdbool.h>
#include <stdint.h>

/* Abstract accelerometer levels (mapped to the product's enum accel_level by mocks/impact.c, so that
 * a reordering of the product enum cannot break the scenarios). */
enum sim_level
{
  SIM_LEVEL_UNKNOWN = 0,
  SIM_LEVEL_REST = 1,
  SIM_LEVEL_HELD = 2,
  SIM_LEVEL_SPINNING = 3,
};

int sim_accel_init(void);                 /* 0, or -errno if the scenario made the sensor fail */
int sim_accel_level(void);                /* enum sim_level */
uint32_t sim_accel_level_ms(void);        /* how long (ms) it has been that level */
bool sim_accel_pop(int64_t *time_ms, int32_t *mg, bool *stall); /* oldest queued event */
int32_t sim_accel_peak(void);             /* nominal |a| for the STATUS line */
int32_t sim_accel_min(void);
uint32_t sim_accel_resets(void);          /* sensor stalls so far */

/* The mock layer around shared/uwb.c's uwb_ball_range_round() reports every round here (LOG_RANGE is
 * compiled out of the product build; this is the sim-only equivalent, with exact times). */
void sim_note_range_round(int ret, int count, const uint8_t *slot, const uint32_t *uid, const int32_t *mm,
                          const uint8_t *lives);

#endif

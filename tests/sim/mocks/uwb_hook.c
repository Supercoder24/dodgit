/*
 * shared/uwb.c, UNMODIFIED, plus one sim-only hook: every ranging round the ball runs is reported to the
 * simulator (time, pouches heard, their distances). The product's LOG_RANGE category is compiled out of
 * the firmware (settings.h), and the scenarios need the exact distances to check the DS-TWR math, so
 * the hook sits around the public function instead of inside it: uwb.c's definition is renamed while
 * it is included, and the original name is defined here as a thin wrapper.
 */
#ifndef SIM_UWB_SOURCE /* run.sh: SIM_UWB_C=file tries a patched copy of uwb.c without touching the product tree */
#define SIM_UWB_SOURCE "../../../shared/uwb.c"
#endif
#define uwb_ball_range_round sim_real_range_round
#include SIM_UWB_SOURCE
#undef uwb_ball_range_round

#include "sim_host.h"

int uwb_ball_range_round(struct pouch_range *out, int max)
{
  int n = sim_real_range_round(out, max);
  uint8_t slot[8], lives[8];
  uint32_t uid[8];
  int32_t mm[8];
  int c = n > 8 ? 8 : (n > 0 ? n : 0);

  for (int i = 0; i < c; i++)
  {
    slot[i] = out[i].slot;
    uid[i] = out[i].uid;
    mm[i] = out[i].mm;
    lives[i] = out[i].lives;
  }
  sim_note_range_round(n, c, slot, uid, mm, lives);
  return n;
}

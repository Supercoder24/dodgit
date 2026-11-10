#ifndef DODGIT_UWB_H
#define DODGIT_UWB_H
#include <stdint.h>
#include <stdbool.h>
struct peer_impact { uint32_t id; int64_t time; int32_t mg; };

int uwb_init(void);
/* Zero means a complete valid range; negative means timeout/error/invalid data.
 * Acknowledged hits are reported even if later steps of ranging fail. */
int uwb_ball_range(uint16_t seq, uint32_t hit, uint32_t *ack, int32_t *distance_mm, struct peer_impact *peer);
int uwb_wearable_step(uint32_t *hit, int32_t *distance_mm);
uint32_t uwb_status(void);
#endif

#ifndef DODGIT_HIT_SETTINGS_H
#define DODGIT_HIT_SETTINGS_H

/* Prototype thresholds: tune after checking real distances and impact traces. */
#define HIT_DISTANCE_MM 1000
#define RANGE_WINDOW_MS 75
#define IMPACT_MG 2500
#define WEARABLE_IMPACT_MG 1800
#define CLASSIFY_WAIT_MS RANGE_WINDOW_MS
#define WEARABLE_REPORT_MS 400
#define WEARABLE_COOLDOWN_MS 150
#define HIT_COOLDOWN_MS 1000
#define SAMPLE_PERIOD_MS 5
#define RANGE_PERIOD_MS 40
#define ANTENNA_DELAY 16385
#define RANGE_OFFSET_MM 0 /* measured positive distance bias; subtracted on ball */

#endif

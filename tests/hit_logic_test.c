#include <assert.h>
#include <stdio.h>
#include "logic.h"

int main(void)
{
    int32_t mm;
    /* Independent radio clocks, 100 DTU one-way flight (~469 mm), unequal delays. */
    assert(twr_distance(1000, 1000+200000+200, 1000+200000+200+300000,
                        8000, 8000+200000, 8000+200000+300000+200, &mm));
    assert(mm >= 468 && mm <= 470);
    /* Clock offset and rollover on both radios; same result. */
    uint32_t a = UINT32_MAX - 50000, b = UINT32_MAX - 10000;
    assert(twr_distance(a, a+200200, a+500200, b, b+200000, b+500200, &mm));
    assert(mm >= 468 && mm <= 470);
    assert(!twr_distance(0, 0, 0, 0, 0, 0, &mm));
    assert(!twr_distance(0, 100, 100100, 0, 100000, 100100, &mm));
    assert(!twr_distance(0, 3000000000U, 3000000100U, 0, 100, 200, &mm));
    assert(range_matches(1000, 925, 500));
    assert(range_matches(1000, 1075, 0));
    assert(!range_matches(1000, 924, 100));
    assert(!range_matches(1000, 1076, 100));
    assert(!range_matches(1000, 1000, -1));
    assert(range_matches(1000, 1000, 501));
    assert(range_matches(1000, 1000, 999));
    assert(range_matches(1000, 1000, 1000));
    assert(!range_matches(1000, 1000, 1001));
    struct impact_gate d = {.ready = true};
    assert(impact_sample(&d, 100, 5000, IMPACT_MG, HIT_COOLDOWN_MS)); /* no arming */
    assert(!impact_sample(&d, 110, 5000, IMPACT_MG, HIT_COOLDOWN_MS));
    assert(!impact_sample(&d, 120, 1000, IMPACT_MG, HIT_COOLDOWN_MS));
    assert(!impact_sample(&d, 200, 5000, IMPACT_MG, HIT_COOLDOWN_MS)); /* cooldown */
    assert(impact_sample(&d, 1200, 5000, IMPACT_MG, HIT_COOLDOWN_MS));
    assert(!impact_sample(&d, 2500, 5000, IMPACT_MG, HIT_COOLDOWN_MS)); /* hysteresis */
    assert(!impact_sample(&d, 2510, 1000, IMPACT_MG, HIT_COOLDOWN_MS));
    assert(impact_sample(&d, 2520, 3000, IMPACT_MG, HIT_COOLDOWN_MS));
    puts("hit logic: passed (ranging, 1m boundary, freshness, always-active ball impacts, cooldown)");
}

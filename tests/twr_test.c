/* Host test of the DS-TWR math used by the ball:
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared tests/twr_test.c -lm -o /tmp/twr_test && /tmp/twr_test */
#include <assert.h>
#include <stdio.h>
#include "twr.h"

int main(void)
{
    int32_t mm = -1;
    /* Independent radio clocks, 100 DTU one-way flight (~469 mm), unequal delays. */
    assert(twr_distance(1000, 1000+200000+200, 1000+200000+200+300000,
                        8000, 8000+200000, 8000+200000+300000+200, &mm));
    assert(mm >= 468 && mm <= 470);
    /* Clock offset and 32-bit rollover on both radios; same result. */
    uint32_t a = UINT32_MAX - 50000, b = UINT32_MAX - 10000;
    assert(twr_distance(a, a+200200, a+500200, b, b+200000, b+500200, &mm));
    assert(mm >= 468 && mm <= 470);
    /* Zero flight time -> 0 mm. */
    assert(twr_distance(0, 200000, 500000, 0, 200000, 500000, &mm));
    assert(mm == 0);
    /* Impossible inputs are rejected, never clamped to "close". */
    mm = -1;
    assert(!twr_distance(0, 0, 0, 0, 0, 0, &mm));               /* zero intervals */
    assert(!twr_distance(0, 100, 100100, 0, 100000, 100100, &mm)); /* negative distance */
    assert(!twr_distance(0, 3000000000U, 3000000100U, 0, 100, 200, &mm)); /* interval > 30 ms */
    assert(mm == -1);
    printf("twr_test: OK\n");
    return 0;
}

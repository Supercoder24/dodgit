#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>
#include "uwb.h"
#include "logic.h"
#include "impact.h"
#include "telemetry.h"

static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
struct range_sample { int64_t time; int32_t mm; bool valid; };
static struct range_sample history[16];

int main(void)
{
    gpio_pin_configure_dt(&blue, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    int rc = telemetry_init();
    if (rc) printk("ERROR,BLE,%d\n", rc);
    rc = impact_init(IMPACT_MG, HIT_COOLDOWN_MS, true);
    if (rc) telemetry("ERROR,SENSOR_INIT,%d", rc);
    rc = uwb_init();
    if (rc) {
        gpio_pin_set_dt(&red, 1);
        while (1) { telemetry("ERROR,UWB_INIT,%d", rc); k_msleep(1000); }
    }
    telemetry("READY,ball,always_active=1,dual_impact=0,threshold_mm=%d", HIT_DISTANCE_MM);
    uint16_t seq = 0;
    uint32_t hit_id = sys_rand32_get() | 1U, pending_hit = 0;
    int64_t pending_until = 0, led_until = 0, log_at = 0, last_range_at = -100000;
    int32_t last_mm = -1;
    unsigned hist_pos = 0, ranges = 0, failures = 0;
    struct impact_event pending = {0};
    bool has_impact = false;

    while (1) {
        int64_t started = k_uptime_get();
        if (pending_hit && started > pending_until) {
            telemetry("HIT_UNACKED,%u", pending_hit);
            pending_hit = 0;
        }
        uint32_t ack;
        int32_t mm;
        struct peer_impact peer;
        rc = uwb_ball_range(seq++, pending_hit, &ack, &mm, &peer);
        int64_t now = k_uptime_get();
        if (pending_hit && ack == pending_hit) {
            telemetry("HIT_ACK,%u", pending_hit);
            pending_hit = 0;
        }
        if (!rc) {
            last_range_at = (started + now) / 2;
            last_mm = mm;
            history[hist_pos++ % ARRAY_SIZE(history)] = (struct range_sample){last_range_at, mm, true};
            ranges++;
        } else failures++;
        gpio_pin_set_dt(&green, now - last_range_at <= 200);

        if (!has_impact && impact_get(&pending)) {
            has_impact = true;
            telemetry("IMPACT,%lld,%d", (long long)pending.time, pending.mg);
        }
        if (has_impact && now >= pending.time + CLASSIFY_WAIT_MS) {
            int32_t nearest_mm = -1;
            for (size_t i = 0; i < ARRAY_SIZE(history); ++i) {
                if (history[i].valid && range_matches(pending.time, history[i].time, history[i].mm) &&
                    (nearest_mm < 0 || history[i].mm < nearest_mm)) nearest_mm = history[i].mm;
            }
            /* The wearable supplies distance/acknowledgement only. Its
             * acceleration never creates or gates a hit on the ball. */
            if (nearest_mm >= 0) {
                if (pending_hit) telemetry("HIT_UNACKED,%u", pending_hit);
                pending_hit = ++hit_id;
                if (!pending_hit) pending_hit = ++hit_id;
                pending_until = now + 1500;
                led_until = now + 1000;
                telemetry("HIT,%u,%lld,%d,%d", pending_hit, (long long)pending.time, nearest_mm, pending.mg);
            } else {
                telemetry("REJECT,%lld,%s,%d", (long long)pending.time,
                          "no_close_fresh_range", pending.mg);
            }
            has_impact = false;
        }
        gpio_pin_set_dt(&red, now < led_until);
        if (now >= log_at) {
            struct impact_status s = impact_status();
            telemetry("DATA,%lld,%d,%lld,%d,%d,%u,%u,%u,%u,%u", (long long)now,
                      last_mm, (long long)(now - last_range_at), telemetry_connected(),
                      s.peak_mg, ranges, failures, telemetry_dropped(), s.errors, s.dropped);
            telemetry("STATE,%lld,active=%d,dual_impact=0,pending_hit=%u", (long long)now,
                      s.ready, pending_hit);
            log_at = now + 200;
        }
        int64_t sleep_ms = RANGE_PERIOD_MS - (k_uptime_get() - started);
        if (sleep_ms > 0) k_msleep(sleep_ms);
    }
}

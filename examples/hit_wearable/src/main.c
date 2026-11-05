#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "uwb.h"
#include "impact.h"
#include "settings.h"

static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);

int main(void)
{
    gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&blue, GPIO_OUTPUT_INACTIVE);
    int rc = impact_init(WEARABLE_IMPACT_MG, WEARABLE_COOLDOWN_MS, false);
    if (rc) {
        printk("ERROR,SENSOR_INIT,%d\n", rc);
        gpio_pin_set_dt(&red, 1);
        return 0;
    }
    rc = uwb_init();
    if (rc) {
        printk("ERROR,UWB_INIT,%d\n", rc);
        gpio_pin_set_dt(&red, 1);
        return 0;
    }
    printk("READY,wearable\n");
    uint32_t last_hit = 0;
    int64_t red_until = 0, last_range = -10000, log_at = 0;
    while (1) {
        uint32_t hit;
        int32_t mm;
        rc = uwb_wearable_step(&hit, &mm);
        int64_t now = k_uptime_get();
        if (hit && hit != last_hit) {
            last_hit = hit;
            red_until = now + 1000;
            printk("HIT,%u\n", hit);
        }
        if (!rc) {
            last_range = now;
            if (now >= log_at) {
                struct impact_status s = impact_status();
                struct impact_report r = impact_latest();
                printk("RANGE,%lld,%d,peak_mg=%d,sensor_errors=%u,impact_id=%u,age_ms=%u\n",
                       (long long)now, mm, s.peak_mg, s.errors, r.id, r.age_ms);
                log_at = now + 200;
            }
        }
        gpio_pin_set_dt(&green, now - last_range <= 200);
        gpio_pin_set_dt(&red, now < red_until);
        k_msleep(1);
    }
}

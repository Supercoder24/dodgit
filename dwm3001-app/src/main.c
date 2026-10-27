
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#include <stdint.h>
#include <stdbool.h>

#define SAMPLE_PERIOD_MS 20
#define DEBOUNCE_MS 40

/* Jerk threshold: 60 m/s^3 */
#define JERK_THRESHOLD_MILLI 60000U

/* Avoid repeated events from one movement */
#define EVENT_COOLDOWN_MS 300

/* Low-pass filter: 1/4 new, 3/4 previous */
#define FILTER_DIVISOR 4

#define LED_NODE DT_ALIAS(led0)
#define BUTTON_NODE DT_ALIAS(sw0)
#define ACCEL_NODE DT_ALIAS(accel0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

static const struct device *const accel =
    DEVICE_DT_GET(ACCEL_NODE);

static uint32_t integer_sqrt(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }

        bit >>= 2;
    }

    return (uint32_t)result;
}

int main(void)
{
    bool streaming = true;
    bool previous_valid = false;
    bool event_armed = true;

    int32_t filtered[3] = {0};
    int32_t previous_filtered[3] = {0};

    int64_t previous_time_ms = 0;
    int64_t last_event_ms = -EVENT_COOLDOWN_MS;

    if (!device_is_ready(accel)) {
        printk("ERROR: Accelerometer not ready\n");
        return 0;
    }

    if (!gpio_is_ready_dt(&led) ||
        !gpio_is_ready_dt(&button)) {
        printk("ERROR: GPIO not ready\n");
        return 0;
    }

    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0 ||
        gpio_pin_configure_dt(&button, GPIO_INPUT) < 0) {
        printk("ERROR: GPIO setup failed\n");
        return 0;
    }

    int initial_button = gpio_pin_get_dt(&button);

    if (initial_button < 0) {
        printk("ERROR: Button read failed\n");
        return 0;
    }

    bool last_raw_button = initial_button != 0;
    bool stable_button = last_raw_button;
    int64_t last_button_change_ms = k_uptime_get();

    printk("\nDWM3001CDK JERK EVENT MONITOR\n");
    printk("STATUS,STARTED\n");
    printk("Press SW2 to stop/resume\n");
    printk("DATA,time_ms,ax_milli,ay_milli,"
           "az_milli,jerk_milli,jerk_detected\n");

    while (1) {
        int64_t now_ms = k_uptime_get();

        /* ---------- BUTTON HANDLING ---------- */

        int button_value = gpio_pin_get_dt(&button);

        if (button_value >= 0) {
            bool raw_button = button_value != 0;

            if (raw_button != last_raw_button) {
                last_raw_button = raw_button;
                last_button_change_ms = now_ms;
            }

            if (raw_button != stable_button &&
                now_ms - last_button_change_ms >=
                    DEBOUNCE_MS) {

                stable_button = raw_button;

                if (stable_button) {
                    streaming = !streaming;

                    /* Reset detection history */
                    previous_valid = false;
                    event_armed = true;

                    gpio_pin_set_dt(
                        &led, streaming ? 1 : 0
                    );

                    if (streaming) {
                        printk("STATUS,RESUMED\n");
                    } else {
                        printk("STATUS,STOPPED\n");
                    }
                }
            }
        }

        if (!streaming) {
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        /* ---------- READ ACCELEROMETER ---------- */

        struct sensor_value xyz[3];

        int ret = sensor_sample_fetch(accel);

        if (ret == 0) {
            ret = sensor_channel_get(
                accel, SENSOR_CHAN_ACCEL_XYZ, xyz
            );
        }

        if (ret < 0) {
            printk("ERROR,SENSOR_READ,%d\n", ret);
            previous_valid = false;
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        /* Acceleration in milli-m/s^2 */
        int32_t raw[3] = {
            (int32_t)sensor_value_to_milli(&xyz[0]),
            (int32_t)sensor_value_to_milli(&xyz[1]),
            (int32_t)sensor_value_to_milli(&xyz[2])
        };

        now_ms = k_uptime_get();

        /* ---------- FIRST SAMPLE ---------- */

        if (!previous_valid) {
            for (int i = 0; i < 3; i++) {
                filtered[i] = raw[i];
                previous_filtered[i] = raw[i];
            }

            previous_time_ms = now_ms;
            previous_valid = true;

            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        /* ---------- LOW-PASS FILTER ---------- */

        for (int i = 0; i < 3; i++) {
            filtered[i] +=
                (raw[i] - filtered[i]) /
                FILTER_DIVISOR;
        }

        /* ---------- CALCULATE JERK ---------- */

        int64_t dt_ms = now_ms - previous_time_ms;

        if (dt_ms > 0 && dt_ms < 1000) {
            int64_t jx =
                ((int64_t)filtered[0] -
                 previous_filtered[0]) *
                1000 / dt_ms;

            int64_t jy =
                ((int64_t)filtered[1] -
                 previous_filtered[1]) *
                1000 / dt_ms;

            int64_t jz =
                ((int64_t)filtered[2] -
                 previous_filtered[2]) *
                1000 / dt_ms;

            uint64_t squared =
                (uint64_t)(jx * jx) +
                (uint64_t)(jy * jy) +
                (uint64_t)(jz * jz);

            uint32_t jerk_milli =
                integer_sqrt(squared);

            /* ---------- EVENT DETECTION ---------- */

            if (jerk_milli <
                JERK_THRESHOLD_MILLI / 2) {
                event_armed = true;
            }

            bool above_threshold =
                jerk_milli >= JERK_THRESHOLD_MILLI;

            bool cooldown_finished =
                now_ms - last_event_ms >=
                EVENT_COOLDOWN_MS;

            if (above_threshold &&
                event_armed &&
                cooldown_finished) {

                printk(
                    "DATA,%u,%d,%d,%d,%u,1\n",
                    (uint32_t)now_ms,
                    filtered[0],
                    filtered[1],
                    filtered[2],
                    jerk_milli
                );

                last_event_ms = now_ms;
                event_armed = false;
            }
        }

        /* ---------- UPDATE PREVIOUS SAMPLE ---------- */

        for (int i = 0; i < 3; i++) {
            previous_filtered[i] = filtered[i];
        }

        previous_time_ms = now_ms;

        k_msleep(SAMPLE_PERIOD_MS);
    }

    return 0;
}

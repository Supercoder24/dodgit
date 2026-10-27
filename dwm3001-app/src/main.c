
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

#include <stdint.h>
#include <stdbool.h>

#define SAMPLE_PERIOD_MS 20
#define DEBOUNCE_MS 40

/* Threshold: 100 m/s^3, stored in milli-units */
#define JERK_THRESHOLD_MILLI 100000U

#define LED_NODE DT_ALIAS(led0)
#define BUTTON_NODE DT_ALIAS(sw0)
#define ACCEL_NODE DT_ALIAS(accel0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

static const struct device *const accel =
    DEVICE_DT_GET(ACCEL_NODE);

/* Integer square root, avoids floating-point printing */
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

    int32_t previous_accel[3] = {0};
    int64_t previous_time_ms = 0;

    if (!device_is_ready(accel)) {
        printk("ERROR: LIS2DH12 sensor not ready\n");
        return 0;
    }

    if (!gpio_is_ready_dt(&led) ||
        !gpio_is_ready_dt(&button)) {
        printk("ERROR: GPIO not ready\n");
        return 0;
    }

    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0 ||
        gpio_pin_configure_dt(&button, GPIO_INPUT) < 0) {
        printk("ERROR: GPIO configuration failed\n");
        return 0;
    }

    int initial_button = gpio_pin_get_dt(&button);

    if (initial_button < 0) {
        printk("ERROR: Cannot read button\n");
        return 0;
    }

    bool last_raw_button = initial_button != 0;
    bool stable_button = last_raw_button;
    int64_t last_button_change_ms = k_uptime_get();

    printk("\nDWM3001CDK REAL-TIME JERK MONITOR\n");
    printk("Accelerometer: LIS2DH12\n");
    printk("Press SW2 to stop or resume streaming\n");
    printk("STATUS,STARTED\n");
    printk("DATA,time_ms,ax_milli,ay_milli,az_milli,"
           "jerk_milli,jerk_detected\n");

    while (1) {
        int64_t now_ms = k_uptime_get();

        /* Read and debounce SW2 button */
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

                /* Toggle only when button is pressed */
                if (stable_button) {
                    streaming = !streaming;
                    previous_valid = false;

                    gpio_pin_set_dt(&led,
                                    streaming ? 1 : 0);

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

        /* Fetch accelerometer measurements */
        struct sensor_value xyz[3];

        int ret = sensor_sample_fetch(accel);

        if (ret == 0) {
            ret = sensor_channel_get(
                accel, SENSOR_CHAN_ACCEL_XYZ, xyz);
        }

        if (ret < 0) {
            printk("ERROR,accelerometer_read,%d\n", ret);
            previous_valid = false;
            k_msleep(SAMPLE_PERIOD_MS);
            continue;
        }

        /* Acceleration converted to milli-m/s^2 */
        int32_t current[3] = {
            (int32_t)sensor_value_to_milli(&xyz[0]),
            (int32_t)sensor_value_to_milli(&xyz[1]),
            (int32_t)sensor_value_to_milli(&xyz[2])
        };

        now_ms = k_uptime_get();

        uint32_t jerk_milli = 0;

        if (previous_valid) {
            int64_t dt_ms =
                now_ms - previous_time_ms;

            if (dt_ms > 0 && dt_ms < 1000) {
                int64_t jx =
                    ((int64_t)current[0] -
                     previous_accel[0]) * 1000 / dt_ms;

                int64_t jy =
                    ((int64_t)current[1] -
                     previous_accel[1]) * 1000 / dt_ms;

                int64_t jz =
                    ((int64_t)current[2] -
                     previous_accel[2]) * 1000 / dt_ms;

                uint64_t squared =
                    (uint64_t)(jx * jx) +
                    (uint64_t)(jy * jy) +
                    (uint64_t)(jz * jz);

                jerk_milli = integer_sqrt(squared);
            }
        }

        int jerk_detected =
            jerk_milli >= JERK_THRESHOLD_MILLI ? 1 : 0;

        printk("DATA,%u,%d,%d,%d,%u,%d\n",
               (uint32_t)now_ms,
               current[0],
               current[1],
               current[2],
               jerk_milli,
               jerk_detected);

        for (int i = 0; i < 3; i++) {
            previous_accel[i] = current[i];
        }

        previous_time_ms = now_ms;
        previous_valid = true;

        k_msleep(SAMPLE_PERIOD_MS);
    }

    return 0;
}

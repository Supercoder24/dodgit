#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        return 0;
    }

    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
        return 0;
    }

    while (1) {
        for (int i = 0; i < 2; i++) {
            gpio_pin_set_dt(&led, 1);
            k_msleep(150);

            gpio_pin_set_dt(&led, 0);
            k_msleep(150);
        }

        k_msleep(1000);
    }

    return 0;
}

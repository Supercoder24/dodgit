/*
 * Host simulator shim for <zephyr/drivers/gpio.h>: the four LEDs and the button of the DWM3001CDK.
 * The product code (ball/src/hw.c, pouch/src/display.c) is compiled unmodified; the simulator
 * records every LED level change with its virtual time and feeds the button from the scenario.
 */
#ifndef SIM_ZEPHYR_GPIO_H
#define SIM_ZEPHYR_GPIO_H

#include <stdbool.h>
#include <stdint.h>

/* DT_ALIAS(led0) etc. expand to the simulated pin number. */
#define SIM_DT_ALIAS_led0 0
#define SIM_DT_ALIAS_led1 1
#define SIM_DT_ALIAS_led2 2
#define SIM_DT_ALIAS_led3 3
#define SIM_DT_ALIAS_sw0 8
#define DT_ALIAS(x) SIM_DT_ALIAS_##x

struct gpio_dt_spec
{
  int pin;
};
#define GPIO_DT_SPEC_GET(node, prop) {.pin = (node)}

#define GPIO_OUTPUT_INACTIVE 0x1
#define GPIO_INPUT 0x2

bool gpio_is_ready_dt(const struct gpio_dt_spec *spec);
int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags);
int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value);
int gpio_pin_get_dt(const struct gpio_dt_spec *spec);

#endif

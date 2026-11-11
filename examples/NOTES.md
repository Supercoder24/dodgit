# Notes
Findings based on Claude and Codex generated example code

### Definitions

`#define VAR_NAME {value}`

Note: Tab for spacing

### GPIO Devices
`decawave_dwm3001cdk.dts`:

```C
aliases {
  sw0 = &button2;
  led0 = &led0;
  led1 = &led1;
  led2 = &led2;
  led3 = &led3;
  watchdog0 = &wdt0;
  accel0 = &lis2dh12;
};

Use `DT_ALIAS(led0)` as shown below
```

### Pins
Configuration:
```C
struct gpio_dt_spec {
	/** GPIO device controlling the pin */
	const struct device *port;
	/** The pin's number on the device */
	gpio_pin_t pin;
	/** The pin's configuration flags as specified in devicetree */
	gpio_dt_flags_t dt_flags;
};
```

Example LED:
```C
/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

if (!gpio_is_ready_dt(&led)) {
  return 0;
}

ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
if (ret < 0) {
  return 0;
}

ret = gpio_pin_toggle_dt(&led);
```

Example Button:
```C
gpio_pin_configure_dt(&button, GPIO_INPUT);

bool raw = gpio_pin_get_dt(&button) > 0; // True = pressed
```

Note:

`DT_ALIAS` to access Device Tree

`gpio_is_ready_dt` returns `true` if ready

`gpio_pin_configure_dt(*gpio_t_spec, GPIO_OUTPUT_INACTIVE (0, LOW) | GPIO_OUTPUT_ACTIVE (1, HIGH))` returns 0 if success, negative on error

`gpio_pin_togge_dt(*gpio_t_spec)` same return vals

### Time
`k_msleep(time_ms)`

### Print
`printf` vs `printk`

### Accelerometer
Set sampling frequency?

```C
struct sensor_value {
	/** Integer part of the value. */
	int32_t val1;
	/** Fractional part of the value (in one-millionth parts). */
	int32_t val2;
};
```

`double sensor_value_to_double(sensor_value)`

Accelerometer returns value in m/s^2. Must divide by g (9.80665) to get Gs.

`device_is_ready` returns true on success

For config, store value in `sensor_value` struct, then apply by passing by reference as seen below. Can even reuse same struct by changing value. No clue what `sensor_g_to_ms2` does.

`static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0))`

Set scale:
`sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &val)` returns 0 on success

Set sampling frequency:
`sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &val)` returns 0 on success

Get sample:
`sensor_sample_fetch` and `sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, *sensor_value[3])`

### UWB
`dwt_...();` to get config info or read predetermined registers. 
E.g. `dwt_readdevid()` or `dwt_check_dev_id()`

Config:
```C
/* Channel 5, 128-symbol preamble, 6.8 Mb/s, no STS: the standard Qorvo
 * example configuration. */
static dwt_config_t config = {
	5,                /* channel */
	DWT_PLEN_128,     /* TX preamble length */
	DWT_PAC8,         /* RX PAC size (matches PLEN_128) */
	9,                /* TX preamble code */
	9,                /* RX preamble code */
	1,                /* SFD type: DW 8-bit */
	DWT_BR_6M8,       /* data rate */
	DWT_PHRMODE_STD,  /* standard PHR */
	DWT_PHRRATE_STD,  /* standard PHR rate */
	(129 + 8 - 8),    /* SFD timeout */
	DWT_STS_MODE_OFF, /* no STS */
	DWT_STS_LEN_64,   /* STS length (unused while STS is off) */
	DWT_PDOA_M0       /* no PDoA */
};
```

Then: `dwt_configure(&config);`

Config TX:
```C
/* TX RF settings: values from Qorvo's default example for channel 5.
 * Writing them does not transmit anything. */
dwt_txconfig_t txconfig = {0x34, 0xFDFDFDFD, 0x0};
dwt_configuretxrf(&txconfig);
```

Calibrate:
```C
/* Antenna delays are stored in the driver and used for ranging. */
dwt_settxantennadelay(16385);
dwt_setrxantennadelay(16385);
```

ID Check Selftest:
```C
dw3000_hw_init();
dw3000_hw_reset();

/* Wait for DW3110 to finish reset */
k_msleep(2000);

int32_t rc = dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);
uint32_t dev_id = dwt_readdevid();
bool ok = rc == DWT_SUCCESS && (dev_id & 0xFFFF0000) == 0xDECA0000;
```

### Bluetooth
Load sampling into second thread.

`K_THREAD_DEFINE(sampler, 1024, sampler_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, 0);`

See `accel_ble`

### Threads and Mutexes
`K_MUTEX_DEFINE`
`K_MSGQ_DEFINE`

`k_msgq_get`
`k_msgq_put`

`k_mutex_lock`
`k_mutex_unlock`

See `hit_common/impact.c`

`k_msleep`
`k_yield`

## Abhi's Code Structure
### `impact.c` and `impact.h`

Thread for sampling accelerometer, adding impacts to a message queue.

### `logic.h`

Logic for ranging and distance calculation and ranging threshold

### `settings.h`

All configured distances, ranges, periods, etc.

### `telemetry.c` and `telemetry.h`

Collecting and logging telemetry, possibly over bluetooth

### `uwb.c` and `uwb.h`

Configure UWB radios and use to perform ranging


## TODO: Investigate
`sensor_g_to_ms2` in accel_ble
Bluetooth in general

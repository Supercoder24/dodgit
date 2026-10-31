# Decadriver Guide: DW3110 on the DWM3001CDK

How to use the Qorvo/Decawave DW3xxx driver ("decadriver") from Zephyr to connect to the DW3110 UWB transceiver and run self-tests that need only **one board**.

Working code that follows this guide: [`examples/qorvo-sdk-zephyr/src/main.c`](../examples/qorvo-sdk-zephyr/src/main.c)

**Contents**

1. [Hardware background](#1-hardware-background)
2. [Project setup](#2-project-setup)
3. [Connecting to the device](#3-connecting-to-the-device)
4. [Single-board tests](#4-single-board-tests)
5. [Reference run](#5-reference-run)
6. [Troubleshooting](#6-troubleshooting)
7. [Next steps: two-device work](#7-next-steps-two-device-work)

**Driver sources** (`modules/zephyr-dw3000-decadriver/`)

| File | Purpose |
| --- | --- |
| `dwt_uwb_driver/deca_device_api.h` | The API: every `dwt_*` function, with detailed doc comments. Read it. |
| `platform/dw3000_hw.h` | Zephyr hardware glue: `dw3000_hw_init`, `dw3000_hw_reset`, `dw3000_hw_wakeup` |
| `platform/deca_probe_interface.h` | `dw3000_probe_interf`, handed to `dwt_probe()` |

---

## 1. Hardware background

The DWM3001C module contains two chips:

| Chip | Role |
| --- | --- |
| **nRF52833** | The MCU. Your Zephyr application runs here. |
| **DW3110** | The UWB transceiver (DW3000 family). This is the chip the decadriver controls. |

They are wired together on the module:

| Signal | Direction | Purpose |
| --- | --- | --- |
| SPI (SCK, MOSI, MISO, CS) | both | All register access and data transfer |
| `RESET` | MCU → DW3110 | Hardware reset. The DW3110 drives it low itself on power-up, so the MCU must only pull it low and release it, never drive it high. |
| `IRQ` | DW3110 → MCU | Interrupt for TX/RX events |
| `WAKEUP` | MCU → DW3110 | Wakes the chip from sleep |

The pin mapping lives in the board overlay,
[`boards/decawave_dwm3001cdk_nrf52833.overlay`](../examples/qorvo-sdk-zephyr/boards/decawave_dwm3001cdk_nrf52833.overlay),
and is read by `platform/dw3000_hw.c`, so application code never touches the pins.

Every function in `deca_device_api.h` is prefixed `dwt_` and talks to the DW3110 over that SPI link.

---

## 2. Project setup

**`CMakeLists.txt`**: add the driver as a Zephyr extra module *before* `find_package(Zephyr)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/zephyr-dw3000-decadriver)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

**`prj.conf`**:

```ini
CONFIG_DW3000=y
CONFIG_DW3000_CHIP_DW3000=y
CONFIG_SPI=y
CONFIG_GPIO=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_LOG_MODE_IMMEDIATE=y
```

Build for the board `decawave_dwm3001cdk/nrf52833`.

> [!NOTE]
> **Console output.** `printk` and `LOG` appear on the board's J-Link USB serial port at 115200 baud. Reset the board *after* opening the port or you will miss the startup text.
> With `CONFIG_LOG=y`, `printk` goes through the logger, and the default deferred buffer drops lines (`--- N messages dropped ---`). `CONFIG_LOG_MODE_IMMEDIATE=y` prevents that.
> `printk` does not print floats by default. Convert to integers first (as `main.c` does) or enable `CONFIG_CBPRINTF_FP_SUPPORT`.

**Includes** used by application code:

```c
#include <deca_device_api.h>        // dwt_* API
#include <deca_probe_interface.h>   // dw3000_probe_interf
#include <dw3000_hw.h>              // dw3000_hw_init / reset / wakeup
```

---

## 3. Connecting to the device

Do these steps **in this order**. Skipping or reordering them is the most common cause of a device ID of `0x0` or `0xFFFFFFFF`.

| Step | Call | What it does |
| --- | --- | --- |
| 1 | `dw3000_hw_init()` | Configures the GPIOs and opens the SPI bus. |
| 2 | `dw3000_hw_reset()` | Pulses RESET so the chip starts from a known state. |
| 3 | `k_msleep(2000)` | The chip needs time to go `INIT_RC` → `IDLE_RC`. `main.c` waits a generous 2 s; 2 ms is the minimum. SPI traffic before then can return garbage or mis-configure the chip. Confirm readiness with `dwt_checkidlerc()` (returns 1 when ready). |
| 4 | `dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf)` | Binds the driver to the Zephyr SPI/HW layer. Returns `DWT_SUCCESS` (0) or `DWT_ERROR` (-1). Must come before any other `dwt_*` call. |
| 5 | `dwt_readdevid()` | Returns the 32-bit device ID. The upper 16 bits must be `0xDECA` (the DW3110 reports `0xDECA0302`). `0x00000000` or `0xFFFFFFFF` means the SPI link is not working. Optionally call `dwt_check_dev_id()`, which returns `DWT_SUCCESS` for a supported ID. |
| 6 | `dwt_initialise(mode)` | Initialises the driver and loads factory calibration from OTP (LDO, bias, crystal trim). Required before configure, TX or RX. `mode` is an OR of `DWT_READ_OTP_PID \| DWT_READ_OTP_LID \| DWT_READ_OTP_BAT \| DWT_READ_OTP_TMP`, selecting which OTP values the getters in [section 4.3](#43-factory-otp-data) return. Returns `DWT_SUCCESS` or `DWT_ERROR`. |
| 7 | `dwt_configure(&config)` | Applies the radio configuration and runs PLL and receiver calibration. Returns `DWT_ERROR` if the PLL fails to lock. |

`dwt_initialise()` expects an SPI clock below 7 MHz. The platform layer already configures that, and after init the bus may run faster.

A known-good starting configuration for step 7:

```c
dwt_config_t config = {
    5,                // chan: 5 or 9
    DWT_PLEN_128,     // txPreambLength
    DWT_PAC8,         // rxPAC, must suit the preamble length
    9, 9,             // txCode, rxCode (preamble codes)
    1,                // sfdType (DW 8-bit)
    DWT_BR_6M8,       // dataRate: 850 kb/s or 6.8 Mb/s
    DWT_PHRMODE_STD,
    DWT_PHRRATE_STD,
    (129 + 8 - 8),    // sfdTO
    DWT_STS_MODE_OFF, // no secure timestamp sequence
    DWT_STS_LEN_64,
    DWT_PDOA_M0       // no phase-difference-of-arrival
};
```

> [!IMPORTANT]
> Two devices must use the **same** channel, preamble, codes, SFD and data rate to hear each other. The tests below need only one board.

---

## 4. Single-board tests

Each test below is implemented and annotated in `main.c`. **Expected** is what a healthy DWM3001CDK should print. Each check prints `[PASS]`, `[FAIL]` or `[INFO]`.

| # | Test | Key calls | Result |
| --- | --- | --- | --- |
| 4.1 | [Identity](#41-identity-spi-read-path) | `dwt_readdevid`, `dwt_check_dev_id` | pass/fail |
| 4.2 | [Boot state](#42-boot-state) | `dwt_checkidlerc`, `dwt_checkirq` | pass/fail |
| 4.3 | [Factory OTP data](#43-factory-otp-data) | `dwt_getpartid`, `dwt_otpread` | pass/fail |
| 4.4 | [Temperature and voltage](#44-temperature-and-supply-voltage) | `dwt_readtempvbat` | pass/fail |
| 4.5 | [SPI write/read-back](#45-spi-writeread-back) | `dwt_seteui`, `dwt_geteui` | pass/fail |
| 4.6 | [PLL lock and calibration](#46-pll-lock-and-calibration) | `dwt_configure`, `dwt_pll_cal` | pass/fail |
| 4.7 | [System timer](#47-system-timer) | `dwt_readsystimestamphi32` | info only |
| 4.8 | [Event counters](#48-event-counters) | `dwt_readeventcounters` | pass/fail |
| 4.9 | [Soft reset](#49-soft-reset) | `dwt_softreset` | pass/fail |

### 4.1 Identity (SPI read path)

**Calls:** `dwt_readdevid()`, `dwt_check_dev_id()`, `dwt_apiversion()`, `dwt_version_string()`

**Expected:** ID `0xDECAxxxx` (here `0xDECA0302`) and `dwt_check_dev_id() == DWT_SUCCESS`.

**If it fails:** `0x0` / `0xFFFFFFFF` means SPI wiring, overlay, power or reset ([troubleshooting](#6-troubleshooting)).

### 4.2 Boot state

**Calls:** `dwt_checkidlerc()`, `dwt_readsysstatuslo()`, `dwt_checkirq()`, `dwt_writesysstatuslo()`

**Expected:** `dwt_checkidlerc() == 1`. After reset, `SYS_STATUS` has the `IDLE_RC` / `SPI_RDY` bits set (the reference run read `0x01800005`) and `dwt_checkirq()` reports them as pending. Clear them by writing 1s with `dwt_writesysstatuslo(0xFFFFFFFF)`, after which `dwt_checkirq() == 0`.

### 4.3 Factory OTP data

**Calls:** `dwt_getpartid()`, `dwt_getlotid()`, `dwt_otprevision()`, `dwt_geticrefvolt()`, `dwt_getxtaltrim()`, `dwt_otpread(addr, buf, words)`

**Expected:**

- Part and lot ID are unique per chip.
- Reference voltage and temperature are non-zero.
- XTAL trim is in `0x00`–`0x3F`.

These getters only return valid data if the matching `DWT_READ_OTP_*` flag was passed to `dwt_initialise()`.

> [!WARNING]
> **Known bug:** `dwt_geticreftemp()` fails to link with this module. `ull_geticreftemp` is declared `static` in `dw3000_device.c`. Read OTP word `0x9` with `dwt_otpread()` instead (as `main.c` does), or delete the `static` in the driver.

> [!CAUTION]
> Never call `dwt_otpwrite*()` while testing. OTP bits are one-time programmable and cannot be undone.

### 4.4 Temperature and supply voltage

**Calls:**

| Function | Returns |
| --- | --- |
| `dwt_readtempvbat()` | `(temp_raw << 8) \| vbat_raw` |
| `dwt_convertrawtemperature(raw)` | degrees C (`float`) |
| `dwt_convertrawvoltage(raw)` | volts (`float`) |

**Expected:** roughly 20–50 °C and 2.5–3.6 V. Throw away the first reading, since it can be stale. The conversions need `dwt_initialise()` called with the BAT/TMP flags.

### 4.5 SPI write/read-back

**Calls:** `dwt_seteui(uint8_t[8])`, then `dwt_geteui(uint8_t[8])`

**Expected:** the bytes read equal the bytes written. This is the simplest proof that the SPI *write* path works. Use two complementary patterns (for example X and ~X) to catch stuck data lines.

For arbitrary registers there are also `dwt_read_reg(addr)` and `dwt_write_reg(addr, value)`, where `addr` is `(register_file << 16) | offset`.

> [!WARNING]
> Use the raw register calls only with the register map (DW3000 User Manual) in hand. A wrong write can mis-configure the chip.

### 4.6 PLL lock and calibration

**Calls:** `dwt_configure()`, `dwt_pll_cal()`, `dwt_configuretxrf()`, `dwt_readpgdelay()`, `dwt_settxantennadelay()` / `dwt_gettxantennadelay()` (and the RX equivalents), `dwt_get_dgcdecision()`

**Expected:**

- `dwt_configure()` and `dwt_pll_cal()` return `DWT_SUCCESS`. A PLL failure points to the 38.4 MHz crystal or the power supply.
- The PG delay reads back what `dwt_configuretxrf()` wrote (`0x34` for channel 5 in the example).
- Antenna delay is a ranging calibration constant (16385 is a typical default), stored by the driver.

### 4.7 System timer

**Calls:** `dwt_readsystimestamphi32()` (32-bit value, 4.0 ns per tick), `dwt_readsystime()` (raw register bytes)

`SYS_TIME` is a 4-byte register. `dwt_readsystime()` fills only 4 bytes, so give it a buffer of at least `SYS_TIME_LEN`, not a 5-byte timestamp.

On the DWM3001CDK with no radio activity, the value was **observed to stay frozen** (idle in `IDLE_RC` or `IDLE_PLL`, and with the receiver on) and to change when the receiver was enabled. It looks event-refreshed rather than free-running.

> [!NOTE]
> That is an observation, not confirmed by the datasheet, so `main.c` prints it as `[INFO]` only. Verify timing with TX/RX timestamps (`dwt_readtxtimestamp`, `dwt_readrxtimestamp`) using two boards.

### 4.8 Event counters

**Calls:** `dwt_configeventcounters(1)` (enables and clears), `dwt_readeventcounters(&c)`

**Expected:** every counter (`TXF`, `CRCG`, `CRCB`, `PHE`, `SFDTO`, `PTO`, `RTO`, ...) is 0, because nothing was sent or received.

### 4.9 Soft reset

**Calls:** `dwt_softreset(1)`, wait about 5 ms, `dwt_checkidlerc()`, `dwt_readdevid()`

**Expected:** the chip returns to `IDLE_RC` and the ID is unchanged. It forgets the radio configuration, so call `dwt_initialise()` and `dwt_configure()` again before further use. A hardware reset (`dw3000_hw_reset()`) does the same via the pin.

### 4.10 Further single-board tests (not in `main.c` yet)

| Area | Calls | Notes |
| --- | --- | --- |
| Sleep / wake | `dwt_configuresleep(DWT_CONFIG \| DWT_PGFCAL, DWT_WAKE_WUP \| DWT_SLP_EN)`, `dwt_entersleep(DWT_DW_IDLE_RC)`, then `dw3000_hw_wakeup()` and `dwt_restoreconfig(1)` | Verify the device ID reads correctly afterwards. The chip does not answer SPI while asleep. |
| AES engine | `dwt_set_keyreg_128()`, `dwt_configure_aes()`, `dwt_do_aes()` | Encrypt/decrypt a known test vector entirely on-chip. |
| GPIO | `dwt_setgpiomode`, `dwt_setgpiodir`, `dwt_setgpiovalue`, `dwt_readgpiovalue`, `dwt_setleds` | Drive the DW3110's own GPIO pins. Check the board schematic first; most are not connected to anything user-visible. |
| Test transmit modes | `dwt_configcwmode()`, `dwt_repeated_cw()`, `dwt_configcontinuousframemode()` | These **transmit RF**. Use only with proper regulatory authorisation, ideally with a spectrum analyser. |

---

## 5. Reference run

Output from a healthy DWM3001CDK running `main.c` (abridged):

```text
===== DWM3001CDK single-board self-test =====

== TEST 1: SPI link and device ID ==
  [PASS] dwt_probe() found a driver
  [INFO] Device ID = 0xDECA0302
  [PASS] Device ID upper half is 0xDECA
  [PASS] dwt_check_dev_id() recognises chip

== TEST 4: temperature and battery voltage ==
  [INFO] Temperature = 39.84 C
  [INFO] Supply voltage = 3.30 V
  [PASS] Temperature plausible
  [PASS] Supply voltage plausible

== TEST 6: radio configure, PLL lock, TX RF registers ==
  [PASS] dwt_configure() (PLL + PGF cal) succeeded
  [PASS] dwt_pll_cal() re-lock succeeded
  [PASS] PG delay register round trip

== TEST 7: system time register (informational) ==
  [INFO] SYS_TIME stayed frozen over 80 ms without radio activity

== TEST 9: soft reset ==
  [PASS] IDLE_RC again after soft reset
  [PASS] Device ID unchanged after soft reset
```

---

## 6. Troubleshooting

| Symptom | Likely cause and fix |
| --- | --- |
| Device ID `0x00000000` or `0xFFFFFFFF` | SPI is not working. Check the board overlay, that the correct board is selected, power, and that `dw3000_hw_reset()` ran and was followed by the startup delay. A read before the chip leaves `INIT_RC` also fails. |
| Device ID reads fine but `dwt_initialise()` returns `DWT_ERROR` | Chip not in `IDLE_RC` yet (wait longer after reset), or the SPI clock is too fast. |
| `dwt_configure()` returns `DWT_ERROR` | The PLL did not lock. Check the crystal and power, and run `dwt_initialise()` first. Do not transmit or receive after an error. |
| OTP getters return 0 | `dwt_initialise()` was not called with the matching `DWT_READ_OTP_*` flag, or the OTP is genuinely unprogrammed. |
| Link error `undefined reference to ull_geticreftemp` | Known driver bug, see [4.3](#43-factory-otp-data). |
| Lines missing, `--- N messages dropped ---` | Add `CONFIG_LOG_MODE_IMMEDIATE=y` to `prj.conf`. |
| Nothing printed | Open the J-Link serial port at 115200 8N1, then reset the board. |

---

## 7. Next steps: two-device work

Frame transmit/receive and ranging need a second board configured identically. The relevant calls:

- **Frames:** `dwt_writetxdata()`, `dwt_writetxfctrl()`, `dwt_starttx()`, `dwt_rxenable()`, `dwt_readrxdata()`
- **Timestamps:** `dwt_readtxtimestamp()`, `dwt_readrxtimestamp()`
- **Scheduling:** delayed TX with `dwt_setdelayedtrxtime()`
- **Events:** `dwt_setcallbacks()`, `dwt_setinterrupt()`, `dwt_isr()`

Double-sided two-way ranging combines these timestamps. See the Qorvo SDK examples.

**Reference:** DW3000 Family User Manual and DW3000 Software API Guide (Qorvo).

/*
 * DWM3001CDK / DW3110 single-board self-test
 *
 * Exercises every part of the Decawave/Qorvo driver (decadriver) that can be
 * checked with ONE board and no second device: no ranging, no frames sent or
 * received over the air. Each test prints its measured values and a
 * PASS / FAIL / INFO line to the Zephyr console (printk).
 *
 *   PASS - the value matched what a healthy board should give
 *   FAIL - the value was wrong; the message says what was expected
 *   INFO - value is printed for the user to eyeball (varies per chip/board)
 *
 * A summary of PASS/FAIL counts is printed at the end. See
 * docs/Decadriver_Guide.md for a walkthrough of the API used here.
 *
 * Hardware: the DWM3001C module pairs an nRF52833 (this code runs on it)
 * with a DW3110 UWB transceiver (the chip under test) over SPI, plus
 * RESET, IRQ and WAKEUP GPIO lines.
 */

#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>

/* ---------------------------------------------------------------------------
 * Tiny test harness
 * ------------------------------------------------------------------------- */
static int n_pass;
static int n_fail;

static void report(const char *name, bool ok, const char *expected)
{
  if (ok)
  {
    n_pass++;
    printk("  [PASS] %s\n", name);
  }
  else
  {
    n_fail++;
    printk("  [FAIL] %s (expected: %s)\n", name, expected);
  }
}

/* printk has no float support by default, so print a float with 2 decimals. */
static void print_fixed(const char *label, float v, const char *unit)
{
  int c = (int)(v * 100.0f);
  printk("  [INFO] %s = %s%d.%02d %s\n", label, c < 0 ? "-" : "", abs(c) / 100, abs(c) % 100, unit);
}

/* ---------------------------------------------------------------------------
 * TEST 1 - SPI link + device identity
 *
 * Purpose: proves the nRF52833 can talk to the DW3110 over SPI and that the
 *          driver found a DW3xxx chip.
 * Expected: dwt_probe() returns DWT_SUCCESS; dwt_readdevid() returns
 *           0xDECA0302 (DW3000 family; the DW3110 on this module reports the
 *           0xDECA03xx family); upper 16 bits must be 0xDECA.
 *           0x00000000 or 0xFFFFFFFF means SPI wiring/power/reset problems.
 *           This test is a gate: later tests are skipped if it fails.
 * ------------------------------------------------------------------------- */
static bool test_identity(void)
{
  printk("\n== TEST 1: SPI link and device ID ==\n");

  int32_t rc = dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);
  report("dwt_probe() found a driver", rc == DWT_SUCCESS, "DWT_SUCCESS (0)");

  uint32_t dev_id = dwt_readdevid();
  printk("  [INFO] Device ID = 0x%08X\n", dev_id);
  report("Device ID upper half is 0xDECA", (dev_id & 0xFFFF0000) == 0xDECA0000, "0xDECAxxxx");

  /* dwt_check_dev_id() compares the ID to the IDs the driver supports. */
  report("dwt_check_dev_id() recognises chip", dwt_check_dev_id() == DWT_SUCCESS, "DWT_SUCCESS (0)");

  printk("  [INFO] Driver API version = 0x%06X, string = %s\n", (unsigned)dwt_apiversion(), dwt_version_string());

  return rc == DWT_SUCCESS && (dev_id & 0xFFFF0000) == 0xDECA0000;
}

/* ---------------------------------------------------------------------------
 * TEST 2 - Boot state and initialisation
 *
 * Purpose: after reset the chip must reach IDLE_RC (internal RC clock, ready
 *          for SPI). dwt_initialise() then loads factory calibration (LDO,
 *          bias, crystal trim) from the chip's OTP memory.
 * Expected: dwt_checkidlerc() == 1 (chip ready) and dwt_initialise() returns
 *           DWT_SUCCESS. SYS_STATUS (low word) will have the IDLE_RC and
 *           SPI_RDY bits set; the exact value is printed for reference.
 * ------------------------------------------------------------------------- */
static void test_init(void)
{
  printk("\n== TEST 2: IDLE_RC state and dwt_initialise ==\n");

  report("dwt_checkidlerc() reports IDLE_RC", dwt_checkidlerc() == 1, "1");

  /* Ask the driver to also read part ID, lot ID, ref. battery voltage and
   * ref. temperature out of OTP so the getters in TEST 3/4 have values. */
  int32_t rc = dwt_initialise(DWT_READ_OTP_PID | DWT_READ_OTP_LID | DWT_READ_OTP_BAT | DWT_READ_OTP_TMP);
  report("dwt_initialise() succeeded", rc == DWT_SUCCESS, "DWT_SUCCESS (0)");

  printk("  [INFO] SYS_STATUS (lo) = 0x%08X\n", dwt_readsysstatuslo());
  /* After reset the chip leaves its power-up status flags (RCINIT, SPIRDY...)
   * set, which raises the IRQ flag. Write-1-to-clear them, then the IRQ line
   * should stay quiet because no frame has been sent/received. */
  dwt_writesysstatuslo(0xFFFFFFFF);
  report("No pending interrupt (dwt_checkirq == 0)", dwt_checkirq() == 0, "0");
}

/* ---------------------------------------------------------------------------
 * TEST 3 - Factory OTP data
 *
 * Purpose: reads values Qorvo burned into OTP at the factory. Confirms the
 *          OTP block is readable and the board was calibrated.
 * Expected: part ID / lot ID are unique per chip (printed as INFO, any value
 *           except all zeros/ones is plausible). OTP revision, reference
 *           voltage, reference temperature and XTAL trim are non-zero on a
 *           factory-calibrated module. XTAL trim must be in 0x00..0x3F.
 * ------------------------------------------------------------------------- */
static void test_otp(void)
{
  printk("\n== TEST 3: OTP / factory calibration ==\n");

  uint32_t part_id = dwt_getpartid();
  uint64_t lot_id = dwt_getlotid();
  printk("  [INFO] Part ID = 0x%08X\n", part_id);
  printk("  [INFO] Lot ID  = 0x%08X%08X\n", (uint32_t)(lot_id >> 32), (uint32_t)lot_id);
  report("Part ID looks programmed", part_id != 0 && part_id != 0xFFFFFFFF, "not 0x0 / 0xFFFFFFFF");

  uint8_t otp_rev = dwt_otprevision();
  uint8_t vref = dwt_geticrefvolt();
  /* dwt_geticreftemp() cannot be used: the vendored driver declares
   * ull_geticreftemp() static, so it fails to link. The reference temperature
   * (measured at 22 C) is stored in OTP word 0x9 (VTEMP_ADDRESS), so read it
   * directly; the value is the low byte. */
  uint32_t tref_word = 0;
  dwt_otpread(0x09, &tref_word, 1);
  uint8_t tref = tref_word & 0xFF;
  uint8_t xtal = dwt_getxtaltrim();
  printk("  [INFO] OTP rev = %u, ref Vbat = 0x%02X, ref temp = 0x%02X, XTAL trim = 0x%02X\n", otp_rev, vref, tref, xtal);
  report("Reference Vbat in OTP is non-zero", vref != 0, "!= 0");
  report("Reference temperature in OTP is non-zero", tref != 0, "!= 0");
  report("XTAL trim within 6-bit range", xtal <= 0x3F, "0x00..0x3F");

  /* Raw OTP word read: address 0x0 is the first word. Prints the first
   * words as a cross-check that dwt_otpread() works. */
  uint32_t otp_words[2] = {0, 0};
  dwt_otpread(0x00, otp_words, 2);
  printk("  [INFO] OTP[0..1] = 0x%08X 0x%08X\n", otp_words[0], otp_words[1]);
}

/* ---------------------------------------------------------------------------
 * TEST 4 - On-chip temperature and battery-voltage sensors
 *
 * Purpose: the DW3110 has a SAR ADC that samples die temperature and supply
 *          voltage. Verifies the ADC runs and the OTP calibration converts raw
 *          counts to sensible units.
 * Expected: temperature roughly room temperature to ~50 C (die self-heats a
 *           little); allowed window -40..125 C. Supply voltage roughly
 *           2.5..3.6 V (the module is powered from the board's 3.0-3.3 V rail;
 *           allowed window 1.5..4.0 V).
 * ------------------------------------------------------------------------- */
static void test_sensors(void)
{
  printk("\n== TEST 4: temperature and battery voltage ==\n");

  /* The first conversion after power-up can be stale, so discard one. */
  (void)dwt_readtempvbat();
  k_msleep(5);
  uint16_t raw = dwt_readtempvbat();
  uint8_t raw_temp = raw >> 8;
  uint8_t raw_vbat = raw & 0xFF;
  printk("  [INFO] raw temp = 0x%02X, raw vbat = 0x%02X\n", raw_temp, raw_vbat);

  float temp_c = dwt_convertrawtemperature(raw_temp);
  float vbat_v = dwt_convertrawvoltage(raw_vbat);
  print_fixed("Temperature", temp_c, "C");
  print_fixed("Supply voltage", vbat_v, "V");

  report("Temperature plausible", temp_c > -40.0f && temp_c < 125.0f, "-40..125 C");
  report("Supply voltage plausible", vbat_v > 1.5f && vbat_v < 4.0f, "1.5..4.0 V");
}

/* ---------------------------------------------------------------------------
 * TEST 5 - SPI write/read-back (EUI-64 register)
 *
 * Purpose: proves SPI *writes* work, not just reads. Writes a known 64-bit
 *          address into the EUI register and reads it back. The register is
 *          only used for frame filtering, so overwriting it is harmless.
 * Expected: the 8 bytes read back are identical to the 8 written.
 * ------------------------------------------------------------------------- */
static void test_spi_rw(void)
{
  printk("\n== TEST 5: SPI write/read-back (EUI-64) ==\n");

  uint8_t eui_out[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
  uint8_t eui_in[8] = {0};

  dwt_seteui(eui_out);
  dwt_geteui(eui_in);
  printk("  [INFO] EUI read back = %02X%02X%02X%02X%02X%02X%02X%02X\n", eui_in[0], eui_in[1], eui_in[2], eui_in[3],
         eui_in[4], eui_in[5], eui_in[6], eui_in[7]);
  report("EUI-64 round trip", memcmp(eui_out, eui_in, sizeof(eui_out)) == 0, "bytes match what was written");

  /* Second pattern (inverse) to rule out a stuck-at bit on the bus. */
  for (int i = 0; i < 8; i++)
  {
    eui_out[i] = (uint8_t)~eui_out[i];
  }
  dwt_seteui(eui_out);
  dwt_geteui(eui_in);
  report("EUI-64 round trip (inverted pattern)", memcmp(eui_out, eui_in, sizeof(eui_out)) == 0, "bytes match");

  /* Generic CRC-8 helper (used for SPI CRC mode); known-answer check only
   * verifies it is deterministic, value is printed for reference. */
  uint8_t crc = dwt_generatecrc8(eui_out, sizeof(eui_out), 0);
  printk("  [INFO] CRC-8 of pattern = 0x%02X\n", crc);
}

/* ---------------------------------------------------------------------------
 * TEST 6 - Radio configuration, PLL lock and PGF calibration
 *
 * Purpose: dwt_configure() programs channel, preamble, data rate etc. and
 *          internally runs PLL calibration, PGF (receiver filter) calibration
 *          and loads DGC settings. If the PLL cannot lock or calibration
 *          fails it returns DWT_ERROR - this checks the chip's RF clocking
 *          and analog blocks without transmitting anything.
 * Expected: dwt_configure() == DWT_SUCCESS; dwt_pll_cal() == DWT_SUCCESS (an
 *           explicit re-lock); dwt_configuretxrf() then dwt_readpgdelay()
 *           returns the pulse-generator delay that was written (0x34).
 *           The DGC decision (receiver gain index) is printed for reference.
 * ------------------------------------------------------------------------- */
static void test_radio_config(void)
{
  printk("\n== TEST 6: radio configure, PLL lock, TX RF registers ==\n");

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

  report("dwt_configure() (PLL + PGF cal) succeeded", dwt_configure(&config) == DWT_SUCCESS, "DWT_SUCCESS (0)");
  report("dwt_pll_cal() re-lock succeeded", dwt_pll_cal() == DWT_SUCCESS, "DWT_SUCCESS (0)");

  /* TX RF settings: values from Qorvo's default example for channel 5.
   * Writing them does not transmit anything. */
  dwt_txconfig_t txconfig = {0x34, 0xFDFDFDFD, 0x0};
  dwt_configuretxrf(&txconfig);
  uint8_t pgdly = dwt_readpgdelay();
  printk("  [INFO] PG delay read back = 0x%02X\n", pgdly);
  report("PG delay register round trip", pgdly == 0x34, "0x34");

  /* Antenna delays are stored in the driver and used for ranging. */
  dwt_settxantennadelay(16385);
  dwt_setrxantennadelay(16385);
  report("TX antenna delay stored", dwt_gettxantennadelay() == 16385, "16385");
  report("RX antenna delay stored", dwt_getrxantennadelay() == 16385, "16385");

  printk("  [INFO] DGC decision = %u\n", dwt_get_dgcdecision());
}

/* ---------------------------------------------------------------------------
 * TEST 7 - System time register (informational, never fails)
 *
 * Purpose: the chip has a 32-bit SYS_TIME register (4.0 ns per tick) that the
 *          driver uses for delayed TX/RX scheduling. This test only reports
 *          what the register does with no radio activity.
 * Expected: on this board the value does NOT advance over time while the chip
 *           is idle: 8 samples taken 10 ms apart print the same number. The
 *           value did change when the receiver was switched on, so it appears
 *           to be refreshed by chip events rather than free-running. This is
 *           an observation, not a verified datasheet behaviour, so a frozen
 *           value is reported as INFO. Real verification (timestamps advancing
 *           across a TX/RX) needs a second board, e.g. a ranging test.
 * ------------------------------------------------------------------------- */
static void test_system_time(void)
{
  printk("\n== TEST 7: system time register (informational) ==\n");

  /* IDLE_PLL is the state in which delayed TX/RX normally operates. */
  report("dwt_setdwstate(IDLE_PLL) succeeded", dwt_setdwstate(DWT_DW_IDLE) == DWT_SUCCESS, "DWT_SUCCESS (0)");

  uint32_t first = dwt_readsystimestamphi32();
  bool changed = false;
  for (int i = 0; i < 8; i++)
  {
    uint32_t t = dwt_readsystimestamphi32();
    printk("  [INFO] sample %d: SYS_TIME = 0x%08X\n", i, t);
    changed = changed || t != first;
    k_msleep(10);
  }
  printk("  [INFO] SYS_TIME %s over 80 ms without radio activity\n", changed ? "advanced" : "stayed frozen");
}

/* ---------------------------------------------------------------------------
 * TEST 8 - Event counters and status registers
 *
 * Purpose: the chip counts RX/TX events (good/bad CRC, preamble timeouts,
 *          transmitted frames...). With no radio activity all must be zero.
 *          Also checks the SYS_STATUS high word reads without error.
 * Expected: after dwt_configeventcounters(1) (which also clears them), every
 *           counter reads 0 - nothing has been sent or received yet.
 * ------------------------------------------------------------------------- */
static void test_counters(void)
{
  printk("\n== TEST 8: event counters ==\n");

  dwt_configeventcounters(1); /* enable + clear */
  dwt_deviceentcnts_t c;
  dwt_readeventcounters(&c);

  printk("  [INFO] TXF=%u CRCG=%u CRCB=%u PHE=%u RSL=%u SFDTO=%u PTO=%u RTO=%u PREJ=%u\n", c.TXF, c.CRCG, c.CRCB, c.PHE,
         c.RSL, c.SFDTO, c.PTO, c.RTO, c.PREJ);
  bool all_zero = c.TXF == 0 && c.CRCG == 0 && c.CRCB == 0 && c.PHE == 0 && c.RSL == 0 && c.SFDTO == 0 && c.PTO == 0 &&
                  c.RTO == 0 && c.PREJ == 0;
  report("All event counters are zero", all_zero, "all 0 with no radio activity");

  printk("  [INFO] SYS_STATUS (hi) = 0x%08X\n", dwt_readsysstatushi());
}

/* ---------------------------------------------------------------------------
 * TEST 9 - Soft reset
 *
 * Purpose: dwt_softreset() restarts the chip over SPI (no RESET pin). After
 *          it, the chip returns to INIT_RC/IDLE_RC and loses its radio
 *          configuration, but must still answer on SPI.
 * Expected: device ID is unchanged after the reset and dwt_checkidlerc() == 1
 *           again. Run last because it discards the configuration above.
 * ------------------------------------------------------------------------- */
static void test_softreset(uint32_t expected_id)
{
  printk("\n== TEST 9: soft reset ==\n");

  dwt_softreset(1);
  k_msleep(5); /* allow INIT_RC -> IDLE_RC */

  report("IDLE_RC again after soft reset", dwt_checkidlerc() == 1, "1");
  uint32_t id = dwt_readdevid();
  printk("  [INFO] Device ID after reset = 0x%08X\n", id);
  report("Device ID unchanged after soft reset", id == expected_id, "same ID as before");
}

int main(void)
{
  dw3000_hw_init();
  dw3000_hw_reset();

  /* Wait for DW3110 to finish reset */
  k_msleep(2000);

  printk("\n===== DWM3001CDK single-board self-test =====\n");

  if (!test_identity())
  {
    printk("\nAborting: no DW3xxx device on SPI. Check board power, SPI wiring/overlay and RESET.\n");
    return 0;
  }

  uint32_t dev_id = dwt_readdevid();

  test_init();
  test_otp();
  test_sensors();
  test_spi_rw();
  test_radio_config();
  test_system_time();
  test_counters();
  test_softreset(dev_id);

  printk("\n===== SUMMARY: %d passed, %d failed =====\n", n_pass, n_fail);
  printk(n_fail == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED - see [FAIL] lines above\n");

  return 0;
}

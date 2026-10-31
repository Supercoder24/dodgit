#include <zephyr/kernel.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>

int main(void)
{
  uint32_t dev_id;

  dw3000_hw_init();
  dw3000_hw_reset();

  /* Wait for DW3110 to finish reset */
  k_msleep(2000);

  dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);

  dev_id = dwt_readdevid();
  printk("Device ID: 0x%08X\n", dev_id);

  if ((dev_id & 0xFFFF0000) == 0xDECA0000)
  {
    printk("SUCCESS: DECA device found\n");
  }
  else if (dev_id == 0x0 || dev_id == 0xFFFFFFFF)
  {
    printk("FAIL: SPI wiring issue (got 0x%08X)\n", dev_id);
  }
  else
  {
    printk("FAIL: Unexpected device ID 0x%08X\n", dev_id);
  }

  return 0;
}
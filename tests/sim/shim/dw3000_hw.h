/* Host simulator shim for dw3000_hw.h (chip power / reset). */
#ifndef SIM_DW3000_HW_H
#define SIM_DW3000_HW_H

int dw3000_hw_init(void);
void dw3000_hw_reset(void);

#endif

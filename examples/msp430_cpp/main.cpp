/*
  TI MSP430 example (Code Composer Studio or msp430-gcc/Energia). Most
  MSP430 CCS projects are C, but the TI compiler and msp430-gcc both
  support C++ for a main.cpp entry point if you want one, same pattern
  as the other examples here.

  Detected automatically here, no configuration needed:
    - budget: 1 KB (16/32/64-byte blocks) - MSP430 RAM is typically
      128 bytes to 16 KB depending on the exact part, so this is a
      reasonable default; override ECCLES_RT_MEM_SIZE downward for the
      smallest parts (e.g. 256 bytes total) if 1 KB is still too much.
    - locking: __disable_interrupt()/__enable_interrupt(), saving and
      restoring the interrupt-enable state first so a lock taken with
      interrupts already off doesn't get turned back on incorrectly.
*/
extern "C" {
#include "eccles_rtmem.h"
}
#include <msp430.h>

int main(void) {
    WDTCTL = WDTPW | WDTHOLD; /* stop the watchdog */

    eccles_rtmem_init();

    while (1) {
        uint8_t *sample = eccles_rt_malloc(8); /* e.g. one ADC reading */
        if (sample) {
            /* ... fill and use sample ... */
            eccles_rt_free(sample);
        }
        __delay_cycles(100000);
    }
}

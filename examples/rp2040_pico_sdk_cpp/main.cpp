/*
  Raspberry Pi Pico (RP2040/RP2350) example using the Pico C/C++ SDK
  directly (this is exactly as true for the Arduino-Pico core, which
  wraps the same SDK). main.cpp is the SDK's normal entry point.

  RP2040 is dual-core, which is exactly why this platform gets its own
  locking case instead of falling through to the generic Cortex-M
  PRIMASK fallback: disabling interrupts only blocks preemption on the
  core that called it - it does NOT stop the *other* physical core from
  running eccles_rt_malloc/eccles_rt_free at the same moment. The Pico SDK's
  critical_section_t is spinlock-backed and safe across both cores,
  which is what eccles_rtmem.c uses automatically here.

  Detected automatically here, no configuration needed:
    - budget: 20 KB (RP2040 has 264 KB of RAM, well into the "big" default)
    - locking: pico/critical_section.h, safe from both core0 and core1
*/
#include "eccles_rtmem.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

void core1_entry() {
    while (true) {
        uint8_t *buf = eccles_rt_malloc(40);
        if (buf) {
            /* ... core1's work with buf ... */
            eccles_rt_free(buf);
        }
        sleep_ms(5);
    }
}

int main() {
    stdio_init_all();
    eccles_rtmem_init(); /* before launching core1, so both cores see it ready */

    multicore_launch_core1(core1_entry);

    while (true) {
        uint8_t *buf = eccles_rt_malloc(90); /* core0 allocating concurrently with core1 above */
        if (buf) {
            /* ... core0's work with buf ... */
            eccles_rt_free(buf);
        }
        sleep_ms(5);
    }
}

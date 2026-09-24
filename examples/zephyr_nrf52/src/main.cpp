/*
  Zephyr RTOS example, e.g. a Nordic nRF52 board (nRF52832/840) built with
  the nRF Connect SDK / west, though this is really generic Zephyr and
  works the same on any Zephyr-supported chip. Zephyr application code is
  commonly C, but src/main.cpp works fine too with CONFIG_CPLUSPLUS=y in
  prj.conf.

  Detected automatically here, no configuration needed:
    - budget: 20 KB - nRF52 typically has 64 KB+ RAM, in the "big" default.
    - locking: a real k_mutex, since __ZEPHYR__ is defined by the Zephyr
      build system - safe to call eccles_rt_malloc/eccles_rt_free from multiple threads.
*/
extern "C" {
#include "eccles_rtmem.h"
}
#include <zephyr/kernel.h>

void sensor_thread(void *, void *, void *) {
    while (1) {
        uint8_t *sample = eccles_rt_malloc(24);
        if (sample) {
            /* ... read a sensor into sample, push to a message queue ... */
            eccles_rt_free(sample);
        }
        k_msleep(100);
    }
}

int main(void) {
    eccles_rtmem_init();

    static k_tid_t tid;
    K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread, NULL, NULL, NULL, 5, 0, 0);
    (void)tid;

    return 0;
}

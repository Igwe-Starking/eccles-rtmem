/*
  mbed OS example - e.g. Arduino Nano 33 BLE / Nano 33 BLE Sense, which
  run the Arduino mbed-based core (a full mbed OS underneath, not the
  plain AVR-style Arduino core). mbed projects are C++ by default.

  Detected automatically here, no configuration needed:
    - budget: 20 KB - the nRF52840 on this board has 256 KB RAM.
    - locking: __MBED__ isn't given its own case in eccles_rtmem.c, so
      this falls through to the generic Cortex-M PRIMASK fallback.
      Masking interrupts also blocks mbed's own RTOS tick, so it's a
      correct (if coarse) critical section here - just not as fine-
      grained as a real rtos::Mutex would be. If you have several mbed
      threads calling eccles_rt_malloc/eccles_rt_free very frequently and want finer-
      grained locking, define your own ECCLES_RT_LOCK_INIT/_LOCK/_UNLOCK/
      _MUTEX_T wrapping mbed's rtos::Mutex before including this header.
*/
#include "mbed.h"
extern "C" {
#include "eccles_rtmem.h"
}

Thread bleThread;

void ble_task() {
    while (true) {
        uint8_t *packet = eccles_rt_malloc(96); /* e.g. one BLE characteristic payload */
        if (packet) {
            /* ... build and send packet ... */
            eccles_rt_free(packet);
        }
        ThisThread::sleep_for(50ms);
    }
}

int main() {
    eccles_rtmem_init();
    bleThread.start(ble_task);

    while (true) {
        ThisThread::sleep_for(1s);
    }
}

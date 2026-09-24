/*
  ECCLES_RT_MEM_NO_LOCK example - for firmware you know is genuinely
  single-threaded: no RTOS, and eccles_rt_malloc/eccles_rt_free are only ever called from
  main()'s superloop, never from an ISR. This strips out the locking
  entirely (no mutex object, no interrupt masking), which matters on the
  smallest 8-bit parts where even a couple of instructions of overhead
  per call is worth avoiding.

  Don't reach for this if any ISR ever touches the pools (a UART receive
  interrupt handing a buffer to the main loop, for instance) - you'd want
  the platform's normal auto-detected locking (or your own) for that,
  not this.
*/
#define ECCLES_RT_MEM_NO_LOCK
#include "eccles_rtmem.h"

int main(void) {
    eccles_rtmem_init();

    for (;;) {
        uint8_t *buf = eccles_rt_malloc(16);
        if (buf) {
            buf[0] = 0x42;
            /* ... do something with buf, still in the same superloop pass ... */
            eccles_rt_free(buf);
        }
    }
}

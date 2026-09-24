/*
  Minimal Arduino AVR example (Uno/Nano/Mega/Leonardo/ATtiny with the
  optiboot/avr-gcc toolchain).

  Copy eccles_rtmem.h and eccles_rtmem.c into this sketch folder (or into a
  library folder named eccles-rtmem) before building. __AVR__ is defined
  automatically by the toolchain, so this picks up the 1 KB pool
  (16/32/64-byte blocks) and the cli()/sei() critical section automatically
  - no configuration needed.
*/
#include "eccles_rtmem.h"

void setup() {
  Serial.begin(115200);
  eccles_rtmem_init();

  uint8_t *buf = eccles_rt_malloc(48);
  if (buf) {
    for (int i = 0; i < 48; i++) buf[i] = i;
    Serial.println("allocated and filled a 48-byte buffer");
    eccles_rt_free(buf);
  } else {
    Serial.println("allocation failed");
  }

  eccles_rt_stats_t s = eccles_rt_get_stats();
  Serial.print("pool A used/free: "); Serial.print(s.usedA); Serial.print("/"); Serial.println(s.freeA);
  Serial.print("pool B used/free: "); Serial.print(s.usedB); Serial.print("/"); Serial.println(s.freeB);
  Serial.print("pool C used/free: "); Serial.print(s.usedC); Serial.print("/"); Serial.println(s.freeC);
}

void loop() {
}

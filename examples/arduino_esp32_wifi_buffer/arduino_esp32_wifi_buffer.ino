/*
  Arduino core for ESP32 example (Arduino IDE / PlatformIO "espressif32"
  platform). Unlike the AVR sketch, this is really C++ under the hood
  (.ino files are compiled as C++) and runs on top of FreeRTOS, which the
  Arduino-ESP32 core always brings in.

  Detected automatically here, no configuration needed:
    - budget: 20 KB (64/128/256-byte blocks) - ESP32 isn't in the
      RAM-constrained list, so it gets the "big" default.
    - locking: a real FreeRTOS mutex, because ARDUINO_ARCH_ESP32 (and
      ESP_PLATFORM) are both defined by this core.
*/
#include "eccles_rtmem.h"
#include <WiFi.h>

static uint8_t *packetBuf = NULL;

void setup() {
  Serial.begin(115200);
  eccles_rtmem_init();
}

void loop() {
  /* simulate grabbing a Wi-Fi packet-sized buffer, using it, giving it back -
     the kind of frequent alloc/free this library exists to make safe */
  packetBuf = eccles_rt_malloc(220);
  if (packetBuf) {
    memset(packetBuf, 0xAA, 220);
    eccles_rt_free(packetBuf);
  } else {
    Serial.println("pool exhausted");
  }
  delay(1000);
}

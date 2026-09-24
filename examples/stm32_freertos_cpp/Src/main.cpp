/*
  STM32 + FreeRTOS example (a CubeMX project with FreeRTOS enabled - by
  default CubeMX wires it up through the CMSIS-RTOS2 wrapper, calling
  osThreadNew/osMutexNew/etc, which is what eccles_rtmem.c will detect
  here since cmsis_os2.h is on the include path).

  Detected automatically here, no configuration needed:
    - budget: 1 KB or 20 KB depending on your STM32 line, same as the
      bare-metal example.
    - locking: a real osMutexId_t (CMSIS-RTOS2), so eccles_rt_malloc/eccles_rt_free are
      safe to call from multiple FreeRTOS tasks concurrently.

  This file stands in for Core/Src/main.cpp's application code - only the
  parts relevant to this library are shown, not the full CubeMX-generated
  boilerplate.
*/
extern "C" {
#include "eccles_rtmem.h"
#include "cmsis_os2.h"
}

extern "C" void StartSensorTask(void *argument) {
    (void)argument;
    for (;;) {
        uint8_t *sample = eccles_rt_malloc(16); /* e.g. one sensor reading */
        if (sample) {
            /* ... fill sample, push to a queue, whatever ... */
            eccles_rt_free(sample);
        }
        osDelay(50);
    }
}

extern "C" void StartLoggerTask(void *argument) {
    (void)argument;
    for (;;) {
        uint8_t *line = eccles_rt_malloc(120); /* e.g. one formatted log line */
        if (line) {
            /* ... format and write line out over UART/SD/etc ... */
            eccles_rt_free(line);
        }
        osDelay(200);
    }
}

/* eccles_rtmem_init() is called once from CubeMX's MX_FREERTOS_Init() (or
   from main() before osKernelStart()), before either task above starts:
   extern "C" void MX_FREERTOS_Init(void) { eccles_rtmem_init(); ... } */

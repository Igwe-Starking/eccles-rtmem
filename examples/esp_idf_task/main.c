/*
  Minimal ESP-IDF example. Add eccles_rtmem.c to your component's sources
  and eccles_rtmem.h to its include path. ESP_PLATFORM is defined by
  ESP-IDF's build, so this picks up the 20 KB default (64/128/256-byte
  blocks) and the FreeRTOS mutex automatically.
*/
#include "eccles_rtmem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rtmem_example";

void app_main(void) {
    eccles_rtmem_init();

    uint8_t *buf = eccles_rt_malloc(300); /* > 256, so this becomes a 2-block run in pool C */
    if (buf) {
        ESP_LOGI(TAG, "allocated a 300-byte buffer at %p", (void*)buf);
        eccles_rt_free(buf);
    } else {
        ESP_LOGE(TAG, "allocation failed");
    }

    eccles_rt_stats_t s = eccles_rt_get_stats();
    ESP_LOGI(TAG, "A used=%d free=%d  B used=%d free=%d  C used=%d free=%d",
             s.usedA, s.freeA, s.usedB, s.freeB, s.usedC, s.freeC);
}

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "yt_app.h"
#if CONFIG_YT_SELF_TEST
static void selftest_task(void *arg) {
    yt_protocol_selftest();
    ESP_LOGI("selftest", "Protocol PASS");
    yt_router_selftest();
    ESP_LOGI("selftest", "Router PASS");
    yt_audio_selftest();
    ESP_LOGI("selftest", "YUANBAO SELFTEST PASS");
    vTaskDelete(NULL);
}
#endif
void app_main(void) {
#if CONFIG_YT_SELF_TEST
    // Opus uses large temporary stack buffers (codec README recommends ~40 KiB).
    if (xTaskCreate(selftest_task, "selftest", 49152, NULL, 5, NULL) != pdPASS)
        ESP_LOGE("selftest", "Self-test task allocation failed");
    return;
#endif
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("yuanbao", "NVS initialization failed: %s; no automatic settings erasure",
                 esp_err_to_name(err));
        return;
    }
    yt_settings_init();
    err = yt_router_start();
    if (err != ESP_OK) {
        ESP_LOGE("yuanbao", "Router startup: %s", esp_err_to_name(err));
        return;
    }
    yt_audio_start();
    yt_gateway_start();
    yt_console_start();
}

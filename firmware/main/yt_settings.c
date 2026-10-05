#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "yt_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
yt_settings_t yt_settings;
atomic_int yt_mix_effective = 1, yt_mix_requested = 0, yt_volume = 70, yt_ttl = 3;
int64_t yt_now(void) {
    return esp_timer_get_time() / 1000;
}
uint64_t yt_random_id(void) {
    uint64_t n;
    esp_fill_random(&n, sizeof(n));
    return n ? n : 1;
}
void yt_settings_init(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("yuanbao", NVS_READWRITE, &h));
    if (nvs_get_u64(h, "origin", &yt_settings.origin) != ESP_OK) {
        yt_settings.origin = yt_random_id();
        ESP_ERROR_CHECK(nvs_set_u64(h, "origin", yt_settings.origin));
    }
    yt_settings.boot = yt_random_id();
    yt_settings.ttl = CONFIG_YT_TTL;
    yt_settings.volume = 70;
#if CONFIG_YT_RELAY
    yt_settings.relay = true;
#endif
    char channel[40] = CONFIG_YT_CHANNEL;
    size_t len = sizeof(channel);
    nvs_get_str(h, "channel", channel, &len);
    if (!yt_channel_parse(channel, yt_settings.channel)) {
        ESP_LOGE("settings", "Invalid channel");
        abort();
    }
    nvs_get_u8(h, "ttl", &yt_settings.ttl);
    nvs_get_u8(h, "volume", &yt_settings.volume);
    nvs_get_u8(h, "mix", &yt_settings.mix_target);
    if (yt_settings.ttl < 1 || yt_settings.ttl > 16)
        yt_settings.ttl = CONFIG_YT_TTL;
    if (yt_settings.volume > 100)
        yt_settings.volume = 70;
    if (yt_settings.mix_target > CONFIG_YT_VERIFIED_MIX_MAX)
        yt_settings.mix_target = 0;
    atomic_store(&yt_mix_requested, yt_settings.mix_target);
    atomic_store(&yt_volume, yt_settings.volume);
    atomic_store(&yt_ttl, yt_settings.ttl);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}
static void console_task(void *arg) {
    char line[128];
    puts("Settings: status | mix auto | mix N | volume 0..100 | ttl 1..16 | channel NUMBER/UUID "
         "(restarts)");
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char cmd[24], value[64];
        int fields = sscanf(line, "%23s %63s", cmd, value);
        if (fields < 1)
            continue;
        if (!strcmp(cmd, "status")) {
            printf("mix target=%d effective=%d verified=%d volume=%d ttl=%d gateway=%d\n",
                   atomic_load(&yt_mix_requested), atomic_load(&yt_mix_effective),
                   CONFIG_YT_VERIFIED_MIX_MAX, atomic_load(&yt_volume), atomic_load(&yt_ttl),
                   yt_gateway_ready());
            continue;
        }
        if (fields != 2) {
            puts("Expected value");
            continue;
        }
        nvs_handle_t h;
        if (nvs_open("yuanbao", NVS_READWRITE, &h) != ESP_OK)
            continue;
        char *end;
        long v = strtol(value, &end, 10);
        bool valid = false, restart = false;
        const char *key = NULL;
        atomic_int *dest = NULL;
        if (!strcmp(cmd, "mix")) {
            if (!strcmp(value, "auto")) {
                v = 0;
                end = value + strlen(value);
            }
            if (!*end && v >= 0 && v <= CONFIG_YT_VERIFIED_MIX_MAX) {
                valid = true;
                key = "mix";
                dest = &yt_mix_requested;
            }
        } else if (!strcmp(cmd, "volume") && !*end && v >= 0 && v <= 100) {
            valid = true;
            key = "volume";
            dest = &yt_volume;
        } else if (!strcmp(cmd, "ttl") && !*end && v >= 1 && v <= 16) {
            valid = true;
            key = "ttl";
            dest = &yt_ttl;
        } else if (!strcmp(cmd, "channel")) {
            uint8_t ch[16];
            if (yt_channel_parse(value, ch)) {
                valid = nvs_set_str(h, "channel", value) == ESP_OK;
                restart = valid;
            }
        }
        if (key)
            valid = nvs_set_u8(h, key, v) == ESP_OK;
        if (valid)
            valid = nvs_commit(h) == ESP_OK;
        nvs_close(h);
        if (valid && dest)
            atomic_store(dest, v);
        puts(valid ? "Saved" : "Invalid or unsupported setting");
        if (valid && restart) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}
void yt_console_start(void) {
    xTaskCreate(console_task, "settings", 4096, NULL, 2, NULL);
}

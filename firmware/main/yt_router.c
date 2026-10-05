#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yt_app.h"
#include <assert.h>
#include <string.h>

#define FLOW_COUNT 32
#define WINDOW 256
typedef struct {
    bool used, radio_sent, upload_sent, server_seen;
    yt_packet_t packet;
    int64_t at, due;
} entry_t;
typedef struct {
    bool used, have_high;
    yt_key_t key;
    uint32_t high;
    int64_t last, end_at;
    uint32_t end_sample;
    entry_t entries[WINDOW];
} flow_t;
typedef struct {
    yt_key_t key;
    int64_t until;
} tomb_t;
static flow_t *flows;
static tomb_t tombs[256];
static QueueHandle_t inbox;
static SemaphoreHandle_t mutex, tx_done;
static uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
static atomic_int_fast64_t reconnect_at;
static atomic_bool associated, connecting, scan_done;
static flow_t *find_flow(const yt_key_t *key) {
    for (int i = 0; i < FLOW_COUNT; i++)
        if (flows[i].used && yt_key_equal(key, &flows[i].key))
            return &flows[i];
    return NULL;
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        associated = false;
        connecting = false;
        reconnect_at = yt_now() + 5000;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        associated = true;
        connecting = false;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        scan_done = true;
}
static void receive(const esp_now_recv_info_t *info, const uint8_t *data, int n) {
    if (!data || n < 64 || n > 250)
        return;
    yt_event_t event = {.received_ms = yt_now(), .ingress = YT_RADIO};
    if (yt_decode(data, n, &event.packet))
        xQueueSend(inbox, &event, 0);
}
static void sent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    xSemaphoreGive(tx_done);
}
bool yt_route(const yt_packet_t *p, int64_t at, int ingress) {
    yt_event_t e = {.packet = *p, .received_ms = at, .ingress = ingress};
    return inbox && xQueueSend(inbox, &e, 0) == pdTRUE;
}
bool yt_claim_upload(const yt_packet_t *p) {
    bool ok = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    flow_t *f = find_flow(&p->key);
    if (f) {
        entry_t *e = &f->entries[p->seq % WINDOW];
        if (e->used && e->packet.seq == p->seq && !e->upload_sent && !e->server_seen) {
            e->upload_sent = true;
            ok = true;
        }
    }
    xSemaphoreGive(mutex);
    return ok;
}
static void process(yt_event_t *event) {
    int64_t now = yt_now(), delta = now - event->received_ms;
    yt_packet_t *p = &event->packet;
    if (delta < 0 || delta + p->age >= 300 || memcmp(p->key.channel, yt_settings.channel, 16))
        return;
    p->age += delta;
    event->received_ms = now;
    // A copied persistent identity is not a second microphone of the same source.
    if (p->key.origin == yt_settings.origin && p->key.boot != yt_settings.boot) {
        ESP_LOGW("router", "Source identity conflict");
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < 256; i++)
        if (tombs[i].until > now && yt_key_equal(&p->key, &tombs[i].key)) {
            xSemaphoreGive(mutex);
            return;
        }
    flow_t *f = find_flow(&p->key);
    if (!f) {
        for (int i = 0; i < FLOW_COUNT; i++)
            if (!flows[i].used) {
                f = &flows[i];
                memset(f, 0, sizeof(*f));
                f->used = true;
                f->key = p->key;
                break;
            }
    }
    if (!f) {
        xSemaphoreGive(mutex);
        return;
    }
    if (f->end_at && ((p->type == YT_AUDIO && p->sample >= f->end_sample) ||
                      (p->type == YT_START) || (p->type == YT_END && p->sample != f->end_sample))) {
        xSemaphoreGive(mutex);
        return;
    }
    if (f->have_high && f->high >= WINDOW && p->seq <= f->high - WINDOW) {
        xSemaphoreGive(mutex);
        return;
    }
    if (!f->have_high || p->seq > f->high) {
        f->high = p->seq;
        f->have_high = true;
    }
    entry_t *e = &f->entries[p->seq % WINDOW];
    bool fresh = !e->used || e->packet.seq != p->seq;
    if (!fresh && !yt_same_content(&e->packet, p)) {
        xSemaphoreGive(mutex);
        return;
    }
    if (fresh) {
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->packet = *p;
        e->at = now;
        e->due = now + (event->ingress == YT_LOCAL ? 0 : esp_random() % 6);
        f->last = now;
        if (p->type == YT_END) {
            f->end_at = now;
            f->end_sample = p->sample;
        }
    }
    if (p->flags & YT_VIA_SERVER) {
        e->server_seen = true;
        e->packet.flags = YT_VIA_SERVER;
        if (!e->radio_sent) {
            e->packet.ttl = p->ttl;
            e->packet.age = p->age;
            e->at = now;
        }
    } else if (!e->server_seen && p->ttl > e->packet.ttl) {
        e->packet.ttl = p->ttl;
    }
    bool upload = !e->server_seen && !e->upload_sent;
    xSemaphoreGive(mutex);
    if (fresh && p->key.origin != yt_settings.origin)
        yt_audio_offer(event);
    if (upload)
        yt_gateway_offer(event);
}
static void maintain(void) {
    int64_t now = yt_now();
    yt_packet_t send_packet;
    bool pending = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < FLOW_COUNT; i++) {
        flow_t *f = &flows[i];
        if (!f->used)
            continue;
        if (f->end_at && now - f->end_at > 300) {
            int slot = -1;
            for (int t = 0; t < 256; t++)
                if (tombs[t].until <= now) {
                    slot = t;
                    break;
                }
            if (slot >= 0) {
                tombs[slot] = (tomb_t){.key = f->key, .until = now + 60000};
                f->used = false;
            }
            // If all tombstones are occupied, retain the flow instead of losing END state.
            continue;
        }
        if (now - f->last > 2000) {
            f->used = false;
            continue;
        }
        for (int j = 0; j < WINDOW; j++) {
            entry_t *e = &f->entries[j];
            if (!e->used || e->radio_sent || !e->packet.ttl || e->due > now)
                continue;
            if (now - e->at > 60 || e->packet.age + now - e->at >= 300) {
                e->radio_sent = true;
                continue;
            }
            if (!yt_settings.relay && e->packet.key.origin != yt_settings.origin)
                continue;
            if (!pending && uxSemaphoreGetCount(tx_done)) {
                send_packet = e->packet;
                send_packet.age += now - e->at;
                send_packet.ttl--;
                e->radio_sent = true;
                pending = true;
            }
        }
    }
    xSemaphoreGive(mutex);
    if (pending && xSemaphoreTake(tx_done, 0) == pdTRUE) {
        uint8_t data[250];
        size_t n = yt_encode(&send_packet, data);
        if (esp_now_send(broadcast, data, n) != ESP_OK)
            xSemaphoreGive(tx_done);
    }
}
static void router_task(void *arg) {
    yt_event_t event;
    for (;;) {
        if (xQueueReceive(inbox, &event, pdMS_TO_TICKS(1)) == pdTRUE)
            process(&event);
        maintain();
        if (scan_done) {
            scan_done = false;
            wifi_ap_record_t ap;
            uint16_t count = 1;
            if (esp_wifi_scan_get_ap_records(&count, &ap) == ESP_OK && count &&
                ap.primary == CONFIG_YT_RADIO_CHANNEL) {
                wifi_config_t cfg = {0};
                strlcpy((char *)cfg.sta.ssid, CONFIG_YT_WIFI_SSID, sizeof(cfg.sta.ssid));
                strlcpy((char *)cfg.sta.password, CONFIG_YT_WIFI_PASSWORD,
                        sizeof(cfg.sta.password));
                cfg.sta.channel = ap.primary;
                cfg.sta.bssid_set = true;
                memcpy(cfg.sta.bssid, ap.bssid, 6);
                if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK ||
                    esp_wifi_connect() != ESP_OK) {
                    connecting = false;
                    reconnect_at = yt_now() + 5000;
                }
            } else {
                connecting = false;
                reconnect_at = yt_now() + 5000;
                esp_wifi_set_channel(CONFIG_YT_RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE);
            }
        }
        if (CONFIG_YT_WIFI_SSID[0] && !associated && !connecting && yt_now() > reconnect_at) {
            // Discover only on the fixed local channel, then bind association to that AP.
            wifi_scan_config_t scan = {.ssid = (uint8_t *)CONFIG_YT_WIFI_SSID,
                                       .channel = CONFIG_YT_RADIO_CHANNEL,
                                       .show_hidden = true};
            connecting = true;
            if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
                connecting = false;
                reconnect_at = yt_now() + 5000;
            }
        }
    }
}
esp_err_t yt_router_start(void) {
    flows = heap_caps_calloc(FLOW_COUNT, sizeof(flow_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!flows) {
        ESP_LOGE("router", "PSRAM required for bounded forwarding windows");
        return ESP_ERR_NO_MEM;
    }
    inbox = xQueueCreate(96, sizeof(yt_event_t));
    mutex = xSemaphoreCreateMutex();
    tx_done = xSemaphoreCreateBinary();
    if (!inbox || !mutex || !tx_done)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(tx_done);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (CONFIG_YT_WIFI_SSID[0]) {
        wifi_config_t wifi = {0};
        strlcpy((char *)wifi.sta.ssid, CONFIG_YT_WIFI_SSID, sizeof(wifi.sta.ssid));
        strlcpy((char *)wifi.sta.password, CONFIG_YT_WIFI_PASSWORD, sizeof(wifi.sta.password));
        wifi.sta.channel = CONFIG_YT_RADIO_CHANNEL;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_YT_RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, broadcast, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(receive));
    ESP_ERROR_CHECK(esp_now_register_send_cb(sent));
    return xTaskCreate(router_task, "router", 6144, NULL, 6, NULL) == pdPASS ? ESP_OK
                                                                             : ESP_ERR_NO_MEM;
}

void yt_router_selftest(void) {
    flows = heap_caps_calloc(FLOW_COUNT, sizeof(flow_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(flows);
    mutex = xSemaphoreCreateMutex();
    assert(mutex);
    assert(yt_channel_parse("1", yt_settings.channel));
    yt_settings.origin = 99;
    yt_settings.boot = 10;
    yt_event_t e = {.received_ms = yt_now(), .ingress = YT_RADIO};
    memcpy(e.packet.key.channel, yt_settings.channel, 16);
    e.packet.key.origin = 1;
    e.packet.key.boot = 2;
    e.packet.key.stream = 3;
    e.packet.type = YT_AUDIO;
    e.packet.seq = 300;
    e.packet.len = 2;
    e.packet.payload[0] = 8;
    process(&e);
    flow_t *f = find_flow(&e.packet.key);
    assert(f);
    entry_t *item = &f->entries[300 % WINDOW];
    assert(item->used && !item->radio_sent && item->packet.ttl == 0);
    e.packet.ttl = 2;
    e.received_ms = yt_now();
    process(&e);
    assert(item->packet.ttl == 2);
    e.packet.flags = 1;
    e.packet.ttl = 3;
    e.received_ms = yt_now();
    process(&e);
    assert(item->server_seen && item->packet.flags == 1 && !yt_claim_upload(&e.packet));
    e.packet.flags = 0;
    e.received_ms = yt_now();
    process(&e);
    assert(item->server_seen);
    e.packet.seq = 45;
    e.packet.sample = 320;
    e.received_ms = yt_now();
    process(&e);
    assert(yt_claim_upload(&e.packet));
    assert(!yt_claim_upload(&e.packet));
    e.packet.seq = 44;
    e.received_ms = yt_now();
    process(&e);
    assert(!yt_claim_upload(&e.packet));
    e.packet.seq = 300;
    e.packet.sample = 640;
    e.received_ms = yt_now();
    process(&e);
    assert(item->packet.sample == 0);
    free(flows);
    flows = NULL;
    vSemaphoreDelete(mutex);
    mutex = NULL;
}

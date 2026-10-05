#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yt_app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int source, type, len;
    uint8_t data[4097];
} ws_message_t;
typedef struct {
    int source, used, total, opcode, frame_start;
    uint8_t data[4097];
} ws_context_t;
typedef struct {
    yt_key_t key;
    uint64_t handle;
    int64_t last, retry;
} proxy_t;
static QueueHandle_t events, uplink;
static esp_websocket_client_handle_t control, media;
static ws_context_t contexts[2] = {{.source = 0}, {.source = 1}};
static atomic_bool ready = false, failed = false;
static uint64_t session, subscription, epoch;
static uint32_t request_id;
static double offset, uncertainty;
static int64_t sync_at;
static proxy_t proxies[32];

bool yt_gateway_ready(void) {
    return atomic_load(&ready);
}
void yt_gateway_offer(const yt_event_t *e) {
    if (uplink && yt_gateway_ready() && !(e->packet.flags & 1)) {
        if (xQueueSend(uplink, e, 0) != pdTRUE) {
            yt_event_t old;
            xQueueReceive(uplink, &old, 0);
            xQueueSend(uplink, e, 0);
        }
    }
}
static void callback(void *arg, esp_event_base_t base, int32_t id, void *event) {
    ws_context_t *ctx = arg;
    esp_websocket_event_data_t *d = event;
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        atomic_store(&failed, true);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA)
        return;
    if (d->op_code >= 8)
        return;
    if (d->payload_len < 0 || d->payload_len > 4096 || d->data_len < 0 || d->payload_offset < 0) {
        atomic_store(&failed, true);
        return;
    }
    if (d->payload_offset == 0) {
        if (d->op_code == 1 || d->op_code == 2) {
            ctx->used = 0;
            ctx->opcode = d->op_code;
        } else if (d->op_code != 0 || !ctx->opcode) {
            atomic_store(&failed, true);
            return;
        }
        ctx->frame_start = ctx->used;
        ctx->total = d->payload_len;
    }
    if (ctx->used - ctx->frame_start != d->payload_offset ||
        d->payload_offset + d->data_len > ctx->total || ctx->used + d->data_len > 4096) {
        atomic_store(&failed, true);
        return;
    }
    memcpy(ctx->data + ctx->used, d->data_ptr, d->data_len);
    ctx->used += d->data_len;
    if (ctx->used - ctx->frame_start == ctx->total && d->fin) {
        ws_message_t msg = {.source = ctx->source, .type = ctx->opcode, .len = ctx->used};
        memcpy(msg.data, ctx->data, ctx->used);
        msg.data[ctx->used] = 0;
        if (xQueueSend(events, &msg, 0) != pdTRUE)
            atomic_store(&failed, true);
        ctx->opcode = 0;
    }
}
static const char *str(cJSON *j, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static uint64_t id_value(cJSON *j, const char *key) {
    uint8_t b[8];
    return yt_hex_parse(str(j, key), b, 8) ? yt_read_be(b, 8) : 0;
}
static void add_id(cJSON *j, const char *key, uint64_t id) {
    char b[17];
    yt_id_hex(id, b);
    cJSON_AddStringToObject(j, key, b);
}
static void capability(cJSON *j) {
    cJSON_AddNumberToObject(j, "max_mix_streams", CONFIG_YT_VERIFIED_MIX_MAX);
    int n = atomic_load(&yt_mix_requested);
    cJSON_AddStringToObject(j, "mix_mode", n ? "manual" : "auto");
    if (n)
        cJSON_AddNumberToObject(j, "requested_mix_streams", n);
    else
        cJSON_AddNullToObject(j, "requested_mix_streams");
    cJSON_AddNumberToObject(j, "effective_mix_streams", atomic_load(&yt_mix_effective));
}
static bool send_json(esp_websocket_client_handle_t client, cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    if (!s)
        return false;
    int n = strlen(s);
    bool ok = esp_websocket_client_send_text(client, s, n, pdMS_TO_TICKS(500)) == n;
    free(s);
    return ok;
}
static void downstream(ws_message_t *msg) {
    if (msg->source != 1 || msg->type != 2 || msg->len < 88 || msg->len > 274 ||
        yt_read_be(msg->data, 8) != session)
        return;
    int64_t now = yt_now();
    double server_now = now + offset;
    uint64_t deadline = yt_read_be(msg->data + 16, 8);
    if (now - sync_at > 20000 || uncertainty > 100 || server_now + uncertainty >= deadline)
        return;
    yt_packet_t p;
    if (!yt_decode(msg->data + 24, msg->len - 24, &p) || p.flags != 1 ||
        memcmp(p.key.channel, yt_settings.channel, 16))
        return;
    int estimated = 300 - (int)(deadline - server_now - uncertainty);
    if (estimated > p.age)
        p.age = estimated;
    if (p.age < 300)
        yt_route(&p, now, YT_SERVER);
}
static cJSON *rpc(cJSON *request) {
    uint32_t rid = ++request_id;
    cJSON_AddNumberToObject(request, "request_id", rid);
    for (int attempt = 0; attempt < 3 && !atomic_load(&failed); attempt++) {
        if (!send_json(control, request)) {
            atomic_store(&failed, true);
            break;
        }
        int64_t until = yt_now() + 2000;
        ws_message_t msg;
        while (yt_now() < until && !atomic_load(&failed)) {
            if (xQueueReceive(events, &msg, pdMS_TO_TICKS(20)) != pdTRUE)
                continue;
            if (msg.source) {
                downstream(&msg);
                continue;
            }
            cJSON *j = cJSON_ParseWithLength((char *)msg.data, msg.len);
            if (!j)
                continue;
            cJSON *id = cJSON_GetObjectItem(j, "request_id");
            if (cJSON_IsNumber(id) && id->valuedouble == rid) {
                cJSON_Delete(request);
                return j;
            }
            cJSON_Delete(j);
        }
    }
    cJSON_Delete(request);
    atomic_store(&failed, true);
    return NULL;
}
static cJSON *command(const char *op) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "op", op);
    return j;
}
static bool success(cJSON *r) {
    return r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"));
}
static bool synchronize(void) {
    int64_t c0 = yt_now();
    cJSON *q = command("TIME_SYNC");
    cJSON_AddNumberToObject(q, "c0", c0);
    cJSON *r = rpc(q);
    int64_t c3 = yt_now();
    bool ok = success(r) && id_value(r, "server_epoch") == epoch;
    cJSON *s1 = cJSON_GetObjectItem(r, "s1"), *s2 = cJSON_GetObjectItem(r, "s2");
    if (ok && cJSON_IsNumber(s1) && cJSON_IsNumber(s2)) {
        double rtt = (c3 - c0) - (s2->valuedouble - s1->valuedouble);
        ok = rtt >= 0 && rtt <= 200;
        if (ok) {
            offset = ((s1->valuedouble - c0) + (s2->valuedouble - c3)) / 2;
            uncertainty = rtt / 2 + 2;
            sync_at = c3;
        }
    } else
        ok = false;
    cJSON_Delete(r);
    return ok;
}
static bool wait_connected(esp_websocket_client_handle_t c) {
    int64_t until = yt_now() + 10000;
    while (!esp_websocket_client_is_connected(c) && !atomic_load(&failed) && yt_now() < until)
        vTaskDelay(pdMS_TO_TICKS(20));
    return esp_websocket_client_is_connected(c);
}
static esp_websocket_client_handle_t make_client(const char *path, int which) {
    char uri[256];
    snprintf(uri, sizeof(uri), "%s%s", CONFIG_YT_SERVER_URI, path);
    esp_websocket_client_config_t cfg = {.uri = uri,
                                         .disable_auto_reconnect = true,
                                         .buffer_size = 4096,
                                         .network_timeout_ms = 500,
                                         .task_stack = 12288,
                                         .crt_bundle_attach = esp_crt_bundle_attach};
    esp_websocket_client_handle_t c = esp_websocket_client_init(&cfg);
    if (c) {
        esp_websocket_register_events(c, WEBSOCKET_EVENT_ANY, callback, &contexts[which]);
        if (esp_websocket_client_start(c) != ESP_OK) {
            esp_websocket_client_destroy(c);
            return NULL;
        }
    }
    return c;
}
static bool connect_server(void) {
    atomic_store(&failed, false);
    for (int i = 0; i < 2; i++) {
        memset(&contexts[i], 0, sizeof(contexts[i]));
        contexts[i].source = i;
    }
    request_id = 0;
    memset(proxies, 0, sizeof(proxies));
    xQueueReset(events);
    xQueueReset(uplink);
    control = make_client("/v1/control", 0);
    if (!control || !wait_connected(control))
        return false;
    cJSON *q = command("HELLO");
    cJSON_AddNumberToObject(q, "version", 1);
    add_id(q, "device_id", yt_settings.origin);
    add_id(q, "boot_id", yt_settings.boot);
    cJSON *profiles = cJSON_AddArrayToObject(q, "profiles");
    cJSON_AddItemToArray(profiles, cJSON_CreateNumber(1));
    capability(q);
    if (CONFIG_YT_SERVER_TOKEN[0])
        cJSON_AddStringToObject(q, "token", CONFIG_YT_SERVER_TOKEN);
    cJSON *r = rpc(q);
    if (!success(r)) {
        cJSON_Delete(r);
        return false;
    }
    session = id_value(r, "session_id");
    epoch = id_value(r, "server_epoch");
    cJSON_Delete(r);
    if (!synchronize())
        return false;
    q = command("JOIN");
    char channel[33];
    yt_hex(yt_settings.channel, 16, channel);
    cJSON_AddStringToObject(q, "channel_id", channel);
    cJSON_AddNumberToObject(q, "profile", 1);
    r = rpc(q);
    if (!success(r)) {
        cJSON_Delete(r);
        return false;
    }
    subscription = id_value(r, "subscription_id");
    char token[64];
    snprintf(token, sizeof(token), "%s", str(r, "media_token"));
    cJSON_Delete(r);
    media = make_client("/v1/media", 1);
    if (!media || !wait_connected(media))
        return false;
    q = command("BIND");
    add_id(q, "session_id", session);
    add_id(q, "subscription_id", subscription);
    cJSON_AddStringToObject(q, "media_token", token);
    bool ok = send_json(media, q);
    cJSON_Delete(q);
    if (!ok)
        return false;
    int64_t until = yt_now() + 2000;
    ws_message_t msg;
    while (yt_now() < until && !atomic_load(&failed)) {
        if (xQueueReceive(events, &msg, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        if (msg.source) {
            downstream(&msg);
            continue;
        }
        r = cJSON_ParseWithLength((char *)msg.data, msg.len);
        ok = r && !strcmp(str(r, "op"), "READY") && id_value(r, "subscription_id") == subscription;
        cJSON_Delete(r);
        if (ok)
            return true;
    }
    return false;
}
static uint64_t register_stream(const yt_event_t *e) {
    int64_t now = yt_now();
    proxy_t *slot = NULL;
    for (int i = 0; i < 32; i++)
        if (proxies[i].last && yt_key_equal(&proxies[i].key, &e->packet.key)) {
            slot = &proxies[i];
            break;
        }
    if (slot && now - slot->last < 900) {
        slot->last = now;
        if (slot->handle)
            return slot->handle;
        if (now < slot->retry)
            return 0;
    }
    if (!slot) {
        for (int i = 0; i < 32; i++)
            if (!proxies[i].last || now - proxies[i].last > 2000) {
                slot = &proxies[i];
                break;
            }
    }
    if (!slot)
        return 0;
    slot->key = e->packet.key;
    slot->last = now;
    slot->handle = 0;
    slot->retry = now + 1000 + esp_random() % 200;
    cJSON *q = command("STREAM_OPEN");
    char ch[33];
    yt_hex(slot->key.channel, 16, ch);
    cJSON_AddStringToObject(q, "channel_id", ch);
    add_id(q, "origin_id", slot->key.origin);
    add_id(q, "boot_id", slot->key.boot);
    add_id(q, "stream_id", slot->key.stream);
    cJSON_AddNumberToObject(q, "profile", 1);
    cJSON_AddNumberToObject(q, "first_observed_sample", e->packet.sample);
    cJSON *r = rpc(q);
    if (success(r))
        slot->handle = id_value(r, "stream_handle");
    cJSON_Delete(r);
    return 0; // Never catch up frames held while registration ran.
}
static void disconnect_all(void) {
    atomic_store(&ready, false);
    if (media) {
        esp_websocket_client_stop(media);
        esp_websocket_client_destroy(media);
        media = NULL;
    }
    if (control) {
        esp_websocket_client_stop(control);
        esp_websocket_client_destroy(control);
        control = NULL;
    }
}
static void gateway_task(void *arg) {
    unsigned backoff = 1;
    for (;;) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {0};
        if (netif)
            esp_netif_get_ip_info(netif, &ip);
        if (!ip.ip.addr) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (connect_server()) {
            atomic_store(&ready, true);
            backoff = 1;
            ESP_LOGI("gateway", "READY");
            int old_target = -1, old_effective = -1;
            int64_t ping = yt_now();
            while (!atomic_load(&failed)) {
                ws_message_t msg;
                while (xQueueReceive(events, &msg, 0) == pdTRUE)
                    if (msg.source)
                        downstream(&msg);
                if (yt_now() - sync_at >= 10000 && !synchronize())
                    break;
                if (yt_now() - ping >= 10000) {
                    cJSON *r = rpc(command("PING"));
                    bool ok = success(r);
                    cJSON_Delete(r);
                    if (!ok)
                        break;
                    ping = yt_now();
                }
                int target = atomic_load(&yt_mix_requested),
                    effective = atomic_load(&yt_mix_effective);
                if (target != old_target || effective != old_effective) {
                    cJSON *q = command("CAPABILITIES_UPDATE");
                    capability(q);
                    cJSON *r = rpc(q);
                    bool ok = success(r);
                    cJSON_Delete(r);
                    if (!ok)
                        break;
                    old_target = target;
                    old_effective = effective;
                }
                yt_event_t e;
                if (xQueueReceive(uplink, &e, pdMS_TO_TICKS(5)) != pdTRUE)
                    continue;
                int64_t now = yt_now();
                if (now - e.received_ms > 100 || e.packet.age + now - e.received_ms >= 300)
                    continue;
                uint64_t handle = register_stream(&e);
                if (!handle)
                    continue;
                now = yt_now();
                int64_t age = e.packet.age + now - e.received_ms;
                if (age >= 300 || !yt_claim_upload(&e.packet))
                    continue;
                e.packet.age = age;
                uint8_t raw[274];
                yt_write_be(raw, session, 8);
                yt_write_be(raw + 8, handle, 8);
                yt_write_be(raw + 16, (uint64_t)(now + offset + 300 - age), 8);
                size_t n = 24 + yt_encode(&e.packet, raw + 24);
                if (esp_websocket_client_send_bin(media, (char *)raw, n, pdMS_TO_TICKS(500)) !=
                    (int)n)
                    break;
            }
        }
        disconnect_all();
        ESP_LOGW("gateway", "Offline; reconnect in %u s", backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000 + esp_random() % 500));
        backoff = backoff < 16 ? backoff * 2 : 30;
    }
}
void yt_gateway_start(void) {
    if (!CONFIG_YT_SERVER_URI[0])
        return;
    events = xQueueCreate(12, sizeof(ws_message_t));
    uplink = xQueueCreate(48, sizeof(yt_event_t));
    if (events && uplink)
        xTaskCreate(gateway_task, "gateway", 14336, NULL, 4, NULL);
}

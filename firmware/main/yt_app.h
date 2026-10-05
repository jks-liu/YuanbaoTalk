#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "yt_protocol.h"
#include <stdatomic.h>
typedef struct {
    yt_packet_t packet;
    int64_t received_ms;
    uint8_t ingress;
} yt_event_t;
enum { YT_LOCAL, YT_RADIO, YT_SERVER };
typedef struct {
    uint8_t channel[16], ttl, volume, mix_target;
    bool relay;
    uint64_t origin, boot;
} yt_settings_t;
extern yt_settings_t yt_settings;
extern atomic_int yt_mix_effective;
extern atomic_int yt_mix_requested;
extern atomic_int yt_volume;
extern atomic_int yt_ttl;
int64_t yt_now(void);
uint64_t yt_random_id(void);
void yt_settings_init(void);
void yt_console_start(void);
esp_err_t yt_router_start(void);
bool yt_route(const yt_packet_t *p, int64_t at, int ingress);
bool yt_claim_upload(const yt_packet_t *p);
void yt_gateway_start(void);
void yt_gateway_offer(const yt_event_t *e);
bool yt_gateway_ready(void);
void yt_audio_start(void);
void yt_audio_offer(const yt_event_t *e);
void yt_protocol_selftest(void);
void yt_router_selftest(void);
void yt_audio_selftest(void);

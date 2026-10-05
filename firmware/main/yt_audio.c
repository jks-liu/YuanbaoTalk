#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ns.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yt_app.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 320
#define MAX_MIX 8
#define JITTER 16
#define REF_SIZE 16384
typedef struct {
    bool valid;
    uint32_t sample;
    uint16_t len;
    uint8_t bytes[186];
} encoded_t;
typedef struct {
    bool active, ended;
    yt_key_t key;
    void *decoder;
    int64_t last, play_at, order;
    uint32_t next_sample, end_sample;
    encoded_t jitter[JITTER];
    int16_t pcm[1024];
    int16_t last_pcm[SAMPLES];
    unsigned read, count, misses;
    float gain;
} voice_t;
static QueueHandle_t incoming;
static voice_t *voices;
static i2s_chan_handle_t tx, rx;
static int16_t reference[REF_SIZE];
static uint64_t ref_written, rx_samples;
static int64_t capture_epoch_us;
static atomic_bool capture_fault;
static portMUX_TYPE ref_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t order;
static void *encoder;
static aec_handle_t *aec[2];
static ns_handle_t ns;
static int chunk, filled, mono_count;
static int16_t *mic[2], *clean[2], *ref, *mono, *filtered;
static int16_t beam_history[2][32];
static unsigned beam_pos;
static int64_t mono_at;
static yt_packet_t outgoing;
static bool transmitting, ptt_locked;
static int64_t ptt_at;
static int64_t processing_sum, processing_peak;
static unsigned processing_frames, overruns;
static const char *TAG = "audio";

static int16_t clip(float x) {
    return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x;
}
static void *aligned(size_t n) {
    return heap_caps_aligned_calloc(16, n, sizeof(int16_t), MALLOC_CAP_8BIT);
}
void yt_audio_offer(const yt_event_t *e) {
    if (incoming && xQueueSend(incoming, e, 0) != pdTRUE) {
        yt_event_t old;
        xQueueReceive(incoming, &old, 0);
        xQueueSend(incoming, e, 0);
    }
}
static void release_voice(voice_t *v) {
    if (v->decoder)
        esp_opus_dec_close(v->decoder);
    memset(v, 0, sizeof(*v));
}
static unsigned voice_count(void) {
    unsigned n = 0;
    for (int i = 0; i < MAX_MIX; i++)
        n += voices[i].active;
    return n;
}
static void accept_audio(yt_event_t *e) {
    yt_packet_t *p = &e->packet;
    int64_t now = yt_now();
    if (p->age + now - e->received_ms >= 300 || p->key.origin == yt_settings.origin)
        return;
    voice_t *v = NULL;
    for (int i = 0; i < MAX_MIX; i++)
        if (voices[i].active && yt_key_equal(&voices[i].key, &p->key)) {
            v = &voices[i];
            break;
        }
    if (!v && p->type == YT_AUDIO && voice_count() < (unsigned)atomic_load(&yt_mix_effective)) {
        for (int i = 0; i < MAX_MIX; i++)
            if (!voices[i].active) {
                v = &voices[i];
                break;
            }
        if (v) {
            esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
            cfg.sample_rate = 16000;
            cfg.channel = 1;
            cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            if (esp_opus_dec_open(&cfg, sizeof(cfg), &v->decoder) != ESP_AUDIO_ERR_OK) {
                memset(v, 0, sizeof(*v));
                return;
            }
            v->active = true;
            v->key = p->key;
            v->next_sample = p->sample;
            v->play_at = now + ((p->flags & 1) ? 120 : 60);
            v->order = ++order;
            v->last = now;
        }
    }
    if (!v)
        return;
    if (p->type == YT_END) {
        v->ended = true;
        v->end_sample = p->sample;
        return;
    }
    if (p->type != YT_AUDIO || p->sample < v->next_sample ||
        (v->ended && p->sample >= v->end_sample))
        return;
    if (p->sample - v->next_sample >= JITTER * 320)
        return;
    encoded_t *slot = &v->jitter[(p->sample / 320) % JITTER];
    if (slot->valid && slot->sample == p->sample)
        return;
    slot->valid = true;
    slot->sample = p->sample;
    slot->len = p->len;
    memcpy(slot->bytes, p->payload, p->len);
    v->last = now;
}
static bool decode_frame(voice_t *v) {
    if (v->ended && v->next_sample >= v->end_sample)
        return false;
    encoded_t *frame = &v->jitter[(v->next_sample / 320) % JITTER];
    bool exists = frame->valid && frame->sample == v->next_sample;
    int16_t pcm[SAMPLES] = {0};
    esp_audio_dec_in_raw_t raw = {
        .buffer = frame->bytes, .len = frame->len, .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE};
    esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
    esp_audio_dec_info_t info = {0};
    // The pinned wrapper has no null-input Opus PLC API. Repeat the last PCM
    // block with a continuous fade to silence over 120 ms; never decode NULL.
    bool decoded =
        exists && esp_opus_dec_decode(v->decoder, &raw, &out, &info) == ESP_AUDIO_ERR_OK &&
        out.decoded_size == sizeof(pcm) && info.channel == 1 && info.sample_rate == 16000;
    if (exists)
        frame->valid = false;
    if (decoded) {
        memcpy(v->last_pcm, pcm, sizeof(pcm));
        v->misses = 0;
    } else {
        for (int i = 0; i < SAMPLES; i++) {
            float gain = fmaxf(0, 1.0f - (v->misses * SAMPLES + i + 1) / (6.0f * SAMPLES));
            pcm[i] = clip(v->last_pcm[i] * gain);
        }
        v->misses++;
    }
    for (int i = 0; i < SAMPLES; i++)
        v->pcm[(v->read + v->count + i) % 1024] = pcm[i];
    v->count += 320;
    v->next_sample += 320;
    return true;
}
static void render(int16_t *out) {
    int64_t now = yt_now();
    float sum[SAMPLES] = {0};
    int capacity = atomic_load(&yt_mix_effective);
    // Release newest streams first. Fade their last buffered samples into this block.
    while (voice_count() > (unsigned)capacity) {
        voice_t *last = NULL;
        for (int i = 0; i < MAX_MIX; i++)
            if (voices[i].active && (!last || voices[i].order > last->order))
                last = &voices[i];
        if (!last)
            break;
        for (unsigned k = 0; k < 320 && k < last->count; k++)
            sum[k] += last->pcm[(last->read + k) % 1024] * last->gain * (1.0f - k / 320.0f);
        release_voice(last);
    }
    for (int i = 0; i < MAX_MIX; i++) {
        voice_t *v = &voices[i];
        if (!v->active)
            continue;
        if (now - v->last > 1000) {
            release_voice(v);
            continue;
        }
        if (now < v->play_at)
            continue;
        unsigned available = 0;
        for (int j = 0; j < JITTER; j++)
            available += v->jitter[j].valid;
        // One-sample corrections per 20 ms compensate drift without unbounded latency.
        unsigned consume = available > 10 ? 321 : (available < 2 && v->count ? 319 : 320);
        while (v->count < consume && v->count <= 704)
            if (!decode_frame(v))
                break;
        if (!v->count && v->ended) {
            release_voice(v);
            continue;
        }
        if (consume > v->count)
            consume = v->count;
        float target = 1.0f / capacity;
        for (unsigned k = 0; k < 320; k++) {
            float pos = k * (consume / 320.0f);
            unsigned a = (unsigned)pos;
            float fraction = pos - a;
            float x = a < v->count ? v->pcm[(v->read + a) % 1024] : 0;
            float y = a + 1 < v->count ? v->pcm[(v->read + a + 1) % 1024] : x;
            v->gain += (target - v->gain) * .01f;
            sum[k] += (x + (y - x) * fraction) * v->gain;
        }
        v->read = (v->read + consume) % 1024;
        v->count -= consume;
    }
    float volume = atomic_load(&yt_volume) / 100.0f;
    for (int k = 0; k < 320; k++)
        out[k] = clip(sum[k] * volume);
}
static bool IRAM_ATTR played(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user) {
    // Capture the actual completed DMA block, including auto-cleared silence on underrun.
    int32_t *samples = event->dma_buf;
    size_t n = event->size / (2 * sizeof(int32_t));
    portENTER_CRITICAL_ISR(&ref_lock);
    for (size_t i = 0; i < n; i++)
        reference[(ref_written + i) % REF_SIZE] = (int16_t)(samples[i * 2] >> 16);
    ref_written += n;
    portEXIT_CRITICAL_ISR(&ref_lock);
    return false;
}
static bool IRAM_ATTR captured(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user) {
    portENTER_CRITICAL_ISR(&ref_lock);
    if (!capture_epoch_us)
        capture_epoch_us = esp_timer_get_time() - (event->size / 8) * 1000000 / 16000;
    portEXIT_CRITICAL_ISR(&ref_lock);
    return false;
}
static bool IRAM_ATTR capture_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event,
                                       void *user) {
    atomic_store(&capture_fault, true);
    return false;
}
static void reference_for(int16_t *dst, unsigned n) {
    portENTER_CRITICAL(&ref_lock);
    for (unsigned i = 0; i < n; i++) {
        int64_t at = (int64_t)(rx_samples + i) - CONFIG_YT_AEC_REFERENCE_DELAY;
        dst[i] = at >= 0 && (uint64_t)at < ref_written && ref_written - (uint64_t)at <= REF_SIZE
                     ? reference[at % REF_SIZE]
                     : 0;
    }
    portEXIT_CRITICAL(&ref_lock);
    rx_samples += n;
}
static void ptt_update(bool pressed) {
    if (!pressed)
        ptt_locked = false;
    if (transmitting && (!pressed || yt_now() - ptt_at >= 60000)) {
        outgoing.type = YT_END;
        outgoing.len = 0;
        outgoing.age = 0;
        outgoing.ttl = atomic_load(&yt_ttl);
        yt_route(&outgoing, yt_now(), YT_LOCAL);
        transmitting = false;
        if (pressed)
            ptt_locked = true;
    }
    if (pressed && !transmitting && !ptt_locked) {
        memset(&outgoing, 0, sizeof(outgoing));
        memcpy(outgoing.key.channel, yt_settings.channel, 16);
        outgoing.key.origin = yt_settings.origin;
        outgoing.key.boot = yt_settings.boot;
        outgoing.key.stream = yt_random_id();
        outgoing.ttl = atomic_load(&yt_ttl);
        outgoing.type = YT_START;
        esp_opus_enc_reset(encoder);
        ptt_at = yt_now();
        transmitting = true;
        yt_route(&outgoing, ptt_at, YT_LOCAL);
        outgoing.seq++;
    }
}
static float __attribute__((unused)) delayed(unsigned channel, unsigned delay_q8) {
    unsigned whole = delay_q8 >> 8;
    float frac = (delay_q8 & 255) / 256.0f;
    float a = beam_history[channel][(beam_pos - whole) & 31],
          b = beam_history[channel][(beam_pos - whole - 1) & 31];
    return a + (b - a) * frac;
}
static void processed_frame(void) {
    if (ns) {
        ns_process(ns, mono, filtered);
        ns_process(ns, mono + 160, filtered + 160);
    } else
        memcpy(filtered, mono, 640);
    if (!transmitting || mono_at < ptt_at)
        return;
    uint8_t bytes[1276];
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)filtered, .len = 640};
    esp_audio_enc_out_frame_t out = {.buffer = bytes, .len = sizeof(bytes)};
    int64_t now = yt_now(), age = now - mono_at;
    if (esp_opus_enc_process(encoder, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes <= 186 &&
        age < 300) {
        outgoing.type = YT_AUDIO;
        outgoing.len = out.encoded_bytes;
        memcpy(outgoing.payload, bytes, out.encoded_bytes);
        outgoing.age = age;
        outgoing.ttl = atomic_load(&yt_ttl);
        yt_route(&outgoing, now, YT_LOCAL);
    }
    outgoing.sample += 320;
    outgoing.seq++;
}
static void capture(const int16_t *left, const int16_t *right, const int16_t *speaker, int64_t at) {
    static int64_t chunk_at;
    for (int n = 0; n < 320; n++) {
        if (!filled)
            chunk_at = at + n / 16;
        mic[0][filled] = left[n];
        mic[1][filled] = right[n];
        ref[filled] = speaker[n];
        filled++;
        if (filled != chunk)
            continue;
        aec_process(aec[0], mic[0], ref, clean[0]);
        aec_process(aec[1], mic[1], ref, clean[1]);
        for (int i = 0; i < chunk; i++) {
            beam_history[0][beam_pos & 31] = clean[0][i];
            beam_history[1][beam_pos & 31] =
                clip(clean[1][i] * (CONFIG_YT_MIC_RIGHT_GAIN_Q12 / 4096.0f));
            float value = clean[0][i];
#if CONFIG_YT_BEAMFORM
            int delay = CONFIG_YT_BEAM_DELAY_Q8;
            value = .5f * (delayed(0, delay > 0 ? delay : 0) + delayed(1, delay < 0 ? -delay : 0));
#endif
            beam_pos++;
            if (!mono_count)
                mono_at = chunk_at + i / 16;
            mono[mono_count++] = clip(value);
            if (mono_count == 320) {
                processed_frame();
                mono_count = 0;
            }
        }
        filled = 0;
    }
}
static bool init_audio(void);
static void __attribute__((unused)) audio_task(void *arg) {
    // Initialization also calls audio libraries; use this task's codec-sized
    // stack and start DMA only when the consuming task is already running.
    if (!init_audio()) {
        ESP_LOGE(TAG, "Audio initialization failed; check configured board pins and resources");
        vTaskDelete(NULL);
        return;
    }
    int32_t input[640], output[640];
    int16_t left[320], right[320], speaker[320], mix[320];
    int64_t window = yt_now(), stable = yt_now();
    unsigned bad = 0;
    bool fault_reported = false;
    bool button = false;
    int64_t changed = 0;
    atomic_store(&yt_mix_effective, CONFIG_YT_VERIFIED_MIX_MAX);
    for (;;) {
        size_t read = 0;
        if (i2s_channel_read(rx, input, sizeof(input), &read, 100) != ESP_OK ||
            read != sizeof(input)) {
            ESP_LOGE(TAG, "RX unavailable; sending suspended");
            ptt_update(false);
            continue;
        }
        portENTER_CRITICAL(&ref_lock);
        int64_t capture_at = capture_epoch_us / 1000 + rx_samples / 16;
        portEXIT_CRITICAL(&ref_lock);
        int64_t start_us = esp_timer_get_time();
        bool pressed = gpio_get_level(CONFIG_YT_PTT_GPIO) == 0;
        if (pressed != button) {
            button = pressed;
            changed = yt_now();
        }
        if (atomic_load(&capture_fault)) {
            if (!fault_reported) {
                ESP_LOGE(TAG,
                         "Capture overflow: sample timeline lost; restart required to transmit");
                fault_reported = true;
            }
            ptt_update(false);
            ptt_locked = true;
        } else if (yt_now() - changed >= 20)
            ptt_update(button);
        yt_event_t event;
        while (xQueueReceive(incoming, &event, 0) == pdTRUE)
            accept_audio(&event);
        render(mix);
        for (int i = 0; i < 320; i++) {
            output[i * 2] = output[i * 2 + 1] = (int32_t)mix[i] * 65536;
            left[i] = clip(input[i * 2] >> CONFIG_YT_MIC_SHIFT);
            right[i] = clip(input[i * 2 + 1] >> CONFIG_YT_MIC_SHIFT);
        }
        reference_for(speaker, 320);
        capture(left, right, speaker, capture_at);
        int64_t busy = esp_timer_get_time() - start_us;
        processing_sum += busy;
        processing_frames++;
        if (busy > processing_peak)
            processing_peak = busy;
        if (busy > 20000) {
            bad++;
            overruns++;
        }
        size_t written = 0;
        if (i2s_channel_write(tx, output, sizeof(output), &written, 20) != ESP_OK ||
            written != sizeof(output)) {
            bad++;
            ESP_LOGW(TAG, "Playback underrun/overrun");
        }
        int64_t now = yt_now();
        if (now - window >= 1000) {
            int current = atomic_load(&yt_mix_effective), request = atomic_load(&yt_mix_requested),
                target = request ? request : CONFIG_YT_VERIFIED_MIX_MAX;
            bool overloaded = bad || processing_peak > 15000;
            if (current > target)
                atomic_store(&yt_mix_effective, target);
            else if (overloaded) {
                if (current > 1)
                    atomic_store(&yt_mix_effective, current - 1);
                else if (bad) {
                    ptt_update(false);
                    ptt_locked = true;
                    ESP_LOGE(TAG, "Minimum full-duplex capacity exceeded");
                }
                stable = now;
            } else if (current < target && now - stable >= 30000 && !transmitting) {
                atomic_store(&yt_mix_effective, current + 1);
                stable = now;
            }
            ESP_LOGI(TAG, "mix=%d/%d avg=%lldus peak=%lldus overruns=%u heap=%u stack_free=%u",
                     atomic_load(&yt_mix_effective), target,
                     processing_frames ? processing_sum / processing_frames : 0, processing_peak,
                     overruns, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            window = now;
            bad = 0;
            processing_sum = processing_peak = processing_frames = 0;
        }
    }
}
static bool __attribute__((unused)) init_audio(void) {
    if (CONFIG_YT_I2S_BCLK < 0 || CONFIG_YT_I2S_WS < 0 || CONFIG_YT_I2S_DIN < 0 ||
        CONFIG_YT_I2S_DOUT < 0 || CONFIG_YT_PTT_GPIO < 0)
        return false;
    int pins[] = {CONFIG_YT_I2S_BCLK, CONFIG_YT_I2S_WS,   CONFIG_YT_I2S_DIN,
                  CONFIG_YT_I2S_DOUT, CONFIG_YT_PTT_GPIO, CONFIG_YT_I2S_MCLK};
    for (int i = 0; i < 6; i++) {
        if (pins[i] < 0)
            continue;
        if (!GPIO_IS_VALID_GPIO(pins[i]))
            return false;
        if ((i == 0 || i == 1 || i == 3 || i == 5) && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))
            return false;
        for (int j = 0; j < i; j++)
            if (pins[i] == pins[j])
                return false;
    }
    aec[0] = aec_create(16000, 4, 1, AEC_MODE_FD_LOW_COST);
    aec[1] = aec_create(16000, 4, 1, AEC_MODE_FD_LOW_COST);
    if (!aec[0] || !aec[1])
        return false;
    chunk = aec_get_chunksize(aec[0]);
    if (chunk <= 0 || chunk > 2048 || chunk != aec_get_chunksize(aec[1]))
        return false;
    for (int i = 0; i < 2; i++) {
        mic[i] = aligned(chunk);
        clean[i] = aligned(chunk);
        if (!mic[i] || !clean[i])
            return false;
    }
    ref = aligned(chunk);
    mono = aligned(320);
    filtered = aligned(320);
    if (!ref || !mono || !filtered)
        return false;
#if CONFIG_YT_NS_ENABLE
    ns = ns_pro_create(10, CONFIG_YT_NS_LEVEL, 16000);
    if (!ns)
        return false;
#endif
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = 16000;
    enc.channel = 1;
    enc.bitrate = 16000;
    enc.complexity = 1;
    if (esp_opus_enc_open(&enc, sizeof(enc), &encoder) != ESP_AUDIO_ERR_OK)
        return false;
    voices = heap_caps_calloc(MAX_MIX, sizeof(voice_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    incoming = xQueueCreate(64, sizeof(yt_event_t));
    if (!voices || !incoming)
        return false;
    gpio_config_t ptt = {.pin_bit_mask = 1ULL << (CONFIG_YT_PTT_GPIO & 63),
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE};
    if (gpio_config(&ptt) != ESP_OK)
        return false;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 160;
    channel.auto_clear = true;
    if (i2s_new_channel(&channel, &tx, &rx) != ESP_OK)
        return false;
    i2s_std_config_t cfg = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
                            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
                            .gpio_cfg = {.mclk = CONFIG_YT_I2S_MCLK,
                                         .bclk = CONFIG_YT_I2S_BCLK,
                                         .ws = CONFIG_YT_I2S_WS,
                                         .dout = CONFIG_YT_I2S_DOUT,
                                         .din = CONFIG_YT_I2S_DIN}};
    if (i2s_channel_init_std_mode(tx, &cfg) != ESP_OK ||
        i2s_channel_init_std_mode(rx, &cfg) != ESP_OK)
        return false;
    i2s_event_callbacks_t callbacks = {.on_sent = played};
    if (i2s_channel_register_event_callback(tx, &callbacks, NULL) != ESP_OK)
        return false;
    i2s_event_callbacks_t rx_callbacks = {.on_recv = captured, .on_recv_q_ovf = capture_overflow};
    if (i2s_channel_register_event_callback(rx, &rx_callbacks, NULL) != ESP_OK)
        return false;
    int32_t silence[1280] = {0};
    size_t loaded;
    if (i2s_channel_preload_data(tx, silence, sizeof(silence), &loaded) != ESP_OK)
        return false;
    if (i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK)
        return false;
    return true;
}
void yt_audio_start(void) {
#if CONFIG_YT_AUDIO_ENABLE
    if (xTaskCreatePinnedToCore(audio_task, "audio", 49152, NULL, 8, NULL, 1) != pdPASS)
        ESP_LOGE(TAG, "Audio task allocation failed");
#else
    ESP_LOGW(TAG, "Audio hardware disabled. Configure YuanbaoTalk in menuconfig.");
#endif
}

void yt_audio_selftest(void) {
    assert(heap_caps_check_integrity_all(true));
    void *enc = NULL, *dec = NULL;
    esp_opus_enc_config_t ec = ESP_OPUS_ENC_CONFIG_DEFAULT();
    ec.sample_rate = 16000;
    ec.channel = 1;
    ec.bitrate = 16000;
    ec.complexity = 1;
    esp_opus_dec_cfg_t dc = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dc.sample_rate = 16000;
    dc.channel = 1;
    dc.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    assert(esp_opus_enc_open(&ec, sizeof(ec), &enc) == ESP_AUDIO_ERR_OK);
    assert(heap_caps_check_integrity_all(true));
    assert(esp_opus_dec_open(&dc, sizeof(dc), &dec) == ESP_AUDIO_ERR_OK);
    assert(heap_caps_check_integrity_all(true));
    int16_t *source = aligned(320), *decoded = aligned(320);
    assert(source && decoded);
    uint8_t bytes[1276];
    for (int i = 0; i < 320; i++)
        source[i] = (i % 32 < 16) ? 2000 : -2000;
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)source, .len = 640};
    esp_audio_enc_out_frame_t out = {.buffer = bytes, .len = sizeof(bytes)};
    assert(esp_opus_enc_process(enc, &in, &out) == ESP_AUDIO_ERR_OK);
    assert(heap_caps_check_integrity_all(true));
    assert(out.encoded_bytes <= 186 && yt_opus_20ms(bytes, out.encoded_bytes));
    esp_audio_dec_in_raw_t raw = {.buffer = bytes, .len = out.encoded_bytes};
    esp_audio_dec_out_frame_t pcm = {.buffer = (uint8_t *)decoded, .len = 640};
    esp_audio_dec_info_t info;
    assert(esp_opus_dec_decode(dec, &raw, &pcm, &info) == ESP_AUDIO_ERR_OK);
    assert(pcm.decoded_size == 640 && info.sample_rate == 16000 && info.channel == 1);
    assert(heap_caps_check_integrity_all(true));
    voice_t *missing = calloc(1, sizeof(*missing));
    assert(missing);
    missing->decoder = dec;
    assert(decode_frame(missing));
    assert(missing->count == 320 && missing->next_sample == 320 && missing->misses == 1);
    for (int i = 0; i < 320; i++)
        assert(missing->pcm[i] == 0);
    for (int i = 0; i < 320; i++)
        missing->last_pcm[i] = 1200;
    missing->misses = 0;
    for (int j = 0; j < 7; j++) {
        missing->count = missing->read = 0;
        assert(decode_frame(missing));
        assert(missing->pcm[0] <= 1200 && missing->pcm[319] >= 0);
    }
    assert(missing->pcm[0] == 0 && missing->pcm[319] == 0);
    free(missing);
    voices = calloc(MAX_MIX, sizeof(*voices));
    assert(voices);
    atomic_store(&yt_mix_effective, 4);
    atomic_store(&yt_volume, 100);
    yt_event_t event = {.received_ms = yt_now()};
    event.packet.type = YT_AUDIO;
    event.packet.len = out.encoded_bytes;
    memcpy(event.packet.payload, bytes, out.encoded_bytes);
    for (int i = 0; i < 5; i++) {
        event.packet.key.origin = 100 + i;
        accept_audio(&event);
    }
    assert(voice_count() == 4);
    for (int i = 0; i < MAX_MIX; i++)
        voices[i].play_at = 0;
    render(decoded);
    for (int i = 0; i < 4; i++)
        assert(voices[i].next_sample == 320 && voices[i].misses == 0);
    atomic_store(&yt_mix_effective, 2);
    render(decoded);
    assert(voice_count() == 2 && voices[0].active && voices[1].active);
    for (int i = 0; i < MAX_MIX; i++)
        release_voice(&voices[i]);
    free(voices);
    voices = NULL;
    esp_opus_enc_close(enc);
    esp_opus_dec_close(dec);
    free(source);
    free(decoded);
    assert(heap_caps_check_integrity_all(true));
    ESP_LOGI(TAG, "Opus, concealment and four-stream mixing PASS; stack_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#if CONFIG_YT_SELF_TEST_AFE
    aec_handle_t *echo = aec_create(16000, 4, 1, AEC_MODE_FD_LOW_COST);
    assert(echo);
    ESP_LOGI(TAG, "AEC created");
    int n = aec_get_chunksize(echo);
    assert(n > 0 && n <= 2048);
    int16_t *a = aligned(n), *b = aligned(n), *c = aligned(n);
    assert(a && b && c);
    aec_process(echo, a, b, c);
    ESP_LOGI(TAG, "AEC processed");
    aec_destroy(echo);
    free(a);
    free(b);
    free(c);
    ns_handle_t noise = ns_pro_create(10, 0, 16000);
    assert(noise);
    ESP_LOGI(TAG, "NS created");
    a = aligned(160);
    b = aligned(160);
    assert(a && b);
    ns_process(noise, a, b);
    ns_destroy(noise);
    free(a);
    free(b);
    assert(heap_caps_check_integrity_all(true));
    ESP_LOGI(TAG, "Audio AEC/NS PASS; stack_free=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#else
    ESP_LOGW(TAG, "AEC/NS processing NOT TESTED; enable YT_SELF_TEST_AFE on supported hardware");
#endif
}

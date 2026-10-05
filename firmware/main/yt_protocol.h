#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define YT_HEADER 64
#define YT_MAX_PACKET 250
#define YT_PAYLOAD 186
#define YT_START 1
#define YT_AUDIO 2
#define YT_END 3
#define YT_VIA_SERVER 1
typedef struct {
    uint8_t channel[16];
    uint64_t origin, boot, stream;
} yt_key_t;
typedef struct {
    yt_key_t key;
    uint32_t seq, sample, age;
    uint16_t len;
    uint8_t type, flags, ttl;
    uint8_t payload[YT_PAYLOAD];
} yt_packet_t;
uint64_t yt_read_be(const uint8_t *p, unsigned n);
void yt_write_be(uint8_t *p, uint64_t v, unsigned n);
bool yt_channel_parse(const char *s, uint8_t channel[16]);
bool yt_hex_parse(const char *s, uint8_t *out, unsigned size);
void yt_hex(const uint8_t *p, unsigned n, char *out);
void yt_id_hex(uint64_t id, char out[17]);
bool yt_key_equal(const yt_key_t *a, const yt_key_t *b);
bool yt_decode(const uint8_t *data, size_t n, yt_packet_t *p);
size_t yt_encode(const yt_packet_t *p, uint8_t out[YT_MAX_PACKET]);
bool yt_same_content(const yt_packet_t *a, const yt_packet_t *b);
/* Validate the Opus TOC duration without decoding untrusted media. */
bool yt_opus_20ms(const uint8_t *p, unsigned n);

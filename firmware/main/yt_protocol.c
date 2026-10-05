#include "yt_protocol.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

uint64_t yt_read_be(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    while (n--)
        v = (v << 8) | *p++;
    return v;
}
void yt_write_be(uint8_t *p, uint64_t v, unsigned n) {
    while (n) {
        p[--n] = (uint8_t)v;
        v >>= 8;
    }
}
static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
bool yt_hex_parse(const char *s, uint8_t *out, unsigned n) {
    if (!s || strlen(s) != 2 * n)
        return false;
    for (unsigned i = 0; i < n; i++) {
        int a = nibble(s[i * 2]), b = nibble(s[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (a << 4) | b;
    }
    return true;
}
void yt_hex(const uint8_t *p, unsigned n, char *out) {
    const char *h = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) {
        out[2 * i] = h[p[i] >> 4];
        out[2 * i + 1] = h[p[i] & 15];
    }
    out[n * 2] = 0;
}
void yt_id_hex(uint64_t id, char out[17]) {
    uint8_t b[8];
    yt_write_be(b, id, 8);
    yt_hex(b, 8, out);
}
bool yt_channel_parse(const char *s, uint8_t out[16]) {
    if (!s || !*s)
        return false;
    memset(out, 0, 16);
    bool numeric = true;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            numeric = false;
    if (numeric) {
        uint64_t n = 0;
        for (; *s; s++) {
            n = n * 10 + (*s - '0');
            if (n > UINT32_MAX)
                return false;
        }
        if (!n)
            return false;
        yt_write_be(out + 12, n, 4);
        return true;
    }
    if (strlen(s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;
    char b[33];
    unsigned k = 0;
    for (unsigned i = 0; i < 36; i++)
        if (i != 8 && i != 13 && i != 18 && i != 23)
            b[k++] = s[i];
    b[k] = 0;
    if (!yt_hex_parse(b, out, 16))
        return false;
    uint8_t any = 0;
    for (int i = 0; i < 12; i++)
        any |= out[i];
    return any != 0;
}
bool yt_key_equal(const yt_key_t *a, const yt_key_t *b) {
    return !memcmp(a->channel, b->channel, 16) && a->origin == b->origin && a->boot == b->boot &&
           a->stream == b->stream;
}
bool yt_opus_20ms(const uint8_t *p, unsigned n) {
    if (!n)
        return false;
    unsigned config = p[0] >> 3, code = p[0] & 3;
    unsigned samples = config >= 16
                           ? (40u << (config & 3))
                           : (config >= 12 ? (160u << (config & 1))
                                           : ((config & 3) == 3 ? 960u : 160u << (config & 3)));
    unsigned count = code == 0 ? 1 : code == 3 ? (n > 1 ? p[1] & 63 : 0) : 2;
    return count && samples * count == 320;
}
bool yt_decode(const uint8_t *d, size_t n, yt_packet_t *p) {
    if (n < 64 || n > 250 || d[0] != 'Y' || d[1] != 'T' || d[2] != 1 || d[3] < 1 || d[3] > 3 ||
        d[4] > 1 || d[5] > 16 || d[6] != 1 || d[7] != 64 || d[58] || d[59])
        return false;
    memset(p, 0, sizeof(*p));
    p->type = d[3];
    p->flags = d[4];
    p->ttl = d[5];
    memcpy(p->key.channel, d + 8, 16);
    p->key.origin = yt_read_be(d + 24, 8);
    p->key.boot = yt_read_be(d + 32, 8);
    p->key.stream = yt_read_be(d + 40, 8);
    p->seq = yt_read_be(d + 48, 4);
    p->sample = yt_read_be(d + 52, 4);
    p->len = yt_read_be(d + 56, 2);
    p->age = yt_read_be(d + 60, 4);
    uint8_t any = 0;
    for (int i = 0; i < 16; i++)
        any |= p->key.channel[i];
    if (!any || !p->key.origin || !p->key.boot || !p->key.stream || p->len != n - 64 ||
        p->age >= 300 || p->sample % 320 || p->sample > 960000)
        return false;
    if (p->type == YT_AUDIO) {
        if (!p->len || p->sample >= 960000 || !yt_opus_20ms(d + 64, p->len))
            return false;
    } else if (p->len || (p->type == YT_START && p->sample))
        return false;
    memcpy(p->payload, d + 64, p->len);
    return true;
}
size_t yt_encode(const yt_packet_t *p, uint8_t d[YT_MAX_PACKET]) {
    if (p->len > YT_PAYLOAD)
        return 0;
    memset(d, 0, 64);
    d[0] = 'Y';
    d[1] = 'T';
    d[2] = 1;
    d[3] = p->type;
    d[4] = p->flags;
    d[5] = p->ttl;
    d[6] = 1;
    d[7] = 64;
    memcpy(d + 8, p->key.channel, 16);
    yt_write_be(d + 24, p->key.origin, 8);
    yt_write_be(d + 32, p->key.boot, 8);
    yt_write_be(d + 40, p->key.stream, 8);
    yt_write_be(d + 48, p->seq, 4);
    yt_write_be(d + 52, p->sample, 4);
    yt_write_be(d + 56, p->len, 2);
    yt_write_be(d + 60, p->age, 4);
    memcpy(d + 64, p->payload, p->len);
    return 64 + p->len;
}
bool yt_same_content(const yt_packet_t *a, const yt_packet_t *b) {
    return yt_key_equal(&a->key, &b->key) && a->seq == b->seq && a->sample == b->sample &&
           a->type == b->type && a->len == b->len && !memcmp(a->payload, b->payload, a->len);
}

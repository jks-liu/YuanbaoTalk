#include "yt_app.h"
#include <assert.h>
#include <string.h>

void yt_protocol_selftest(void) {
    yt_packet_t p = {.type = YT_AUDIO, .ttl = 2, .seq = 1, .len = 2, .payload = {8, 1}};
    assert(yt_channel_parse("0001", p.key.channel));
    p.key.origin = 1;
    p.key.boot = 2;
    p.key.stream = 3;
    uint8_t raw[250];
    size_t n = yt_encode(&p, raw);
    const uint8_t header[] = {0x59, 0x54, 1, 2, 0, 2, 1, 64};
    assert(n == 66 && !memcmp(raw, header, 8));
    assert(raw[23] == 1 && raw[31] == 1 && raw[39] == 2 && raw[47] == 3 && raw[51] == 1 &&
           raw[57] == 2);
    yt_packet_t q;
    assert(yt_decode(raw, n, &q) && yt_same_content(&p, &q));
    q.flags = 1;
    q.ttl = 3;
    q.age = 100;
    assert(yt_same_content(&p, &q));
    raw[5] = 17;
    assert(!yt_decode(raw, n, &q));
    raw[5] = 2;
    raw[4] = 2;
    assert(!yt_decode(raw, n, &q));
    raw[4] = 0;
    assert(!yt_decode(raw, n - 1, &q));
    assert(!yt_channel_parse("0", q.key.channel));
    assert(!yt_channel_parse("4294967296", q.key.channel));
    assert(!yt_channel_parse("00000000-0000-0000-0000-000000000001", q.key.channel));
    assert(yt_channel_parse("5f180b62-879b-41fa-8ee5-ea641f317ba0", q.key.channel));
    assert(!yt_opus_20ms((uint8_t[]){0}, 1));
}

#include "live_pmd.h"
#include "live_stream.h"
#include <Arduino.h>
#include <string.h>

namespace LivePmd {

const char     TAG[4]      = "PMD";
const uint16_t SAMPLE_SIZE = sizeof(Sample);

#define PMD_MIN_LEN     13      // "PMD" + 2 + 3 + 3 + 2

static int hex_nib(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int parse_hex(const uint8_t *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int d = hex_nib(p[i]);
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

static bool decode(const uint8_t *payload, uint16_t len,
                   void *out, uint16_t out_size) {
    if (len < PMD_MIN_LEN) return false;
    if (out_size < sizeof(Sample)) return false;

    // Skip 3-char tag and 2-hex frame sequence (we don't use it).
    const uint8_t *p = payload + 3 + 2;

    int mkp     = parse_hex(p, 3); p += 3;
    int rfl_raw = parse_hex(p, 3); p += 3;
    int lyk     = parse_hex(p, 2);
    if (mkp < 0 || rfl_raw < 0 || lyk < 0) return false;

    // RFL is signed 12-bit (s3): sign-extend to int16.
    int16_t rfl = (int16_t)rfl_raw;
    if (rfl & 0x800) rfl |= 0xF000;

    Sample *s = (Sample *)out;
    s->mkp   = (int16_t)mkp;
    s->rfl   = rfl;
    s->lyk   = (int16_t)lyk;
    s->ts_ms = millis();
    return true;
}

void register_parser() {
    LiveStream::register_stream(TAG, decode, SAMPLE_SIZE);
}

}

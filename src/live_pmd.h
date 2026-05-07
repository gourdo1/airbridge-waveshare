#pragma once
#include <stdint.h>

// PMD live stream: 25 Hz pressure / flow / leak triplets from AirSense.
//
// L-frame payload layout (13 ASCII bytes total):
//   "PMD" <seq:2-hex> <MKP:3-hex unsigned> <RFL:3-hex signed s12> <LYK:2-hex>
//
// Scaling (display only; Sample stores raw values):
//   MKP / 50  -> cmH2O    (mask pressure)
//   RFL / 500 -> L/s      (respiratory flow, sign-extended from 12 bits)
//   LYK / 50  -> L/s      (mask leak)

namespace LivePmd {

struct Sample {
    int16_t  mkp;
    int16_t  rfl;
    int16_t  lyk;
    uint32_t ts_ms;
};

extern const char     TAG[4];          // "PMD\0"
extern const uint16_t SAMPLE_SIZE;     // sizeof(Sample)

// Register the PMD parser with LiveStream. Idempotent: re-registration is
// rejected by the broker but harmless. Call before any consumer subscribes
// to "PMD".
void register_parser();

}

#pragma once
#include <stdint.h>

// TCE live stream: pressure / flow / leak, decoded by the discovered schema.
//
// Scaling (display only; Sample stores raw values):
//   MKP / 50  -> cmH2O    (mask pressure)
//   RFL / 500 -> L/s      (respiratory flow, sign-extended from 12 bits)
//   LYK / 50  -> L/s      (mask leak)

namespace LiveTce {

struct Sample {
    int16_t  mkp;
    int16_t  rfl;
    int16_t  lyk;
    uint32_t ts_ms;
};

extern const char     TAG[4];          // "TCE\0"
extern const uint16_t SAMPLE_SIZE;     // sizeof(Sample)

// Register the TCE parser with LiveStream. Idempotent: re-registration is
// rejected by the broker but harmless. Call before any consumer subscribes
// to "TCE". The broker prepares its schema before enabling the stream.
void register_parser();

}

#include "live_tce.h"
#include "air10_stream.h"
#include "live_stream.h"
#include "airsense_state.h"
#include <Arduino.h>

namespace LiveTce {

const char TAG[4] = "TCE";
const uint16_t SAMPLE_SIZE = sizeof(Sample);

static Air10Stream::Schema schema;
static portMUX_TYPE schema_mux = portMUX_INITIALIZER_UNLOCKED;

static bool prepare() {
    AirSenseState::Identity identity;
    if (!AirSenseState::identity(identity)) {
        AirSenseState::request_refresh();
        return false;
    }
    Air10Stream::Schema resolved = {};
    if (!Air10Stream::resolve_schema(resolved, TAG, identity.mid, identity.vid) ||
        identity.generation != AirSenseState::identity_generation()) return false;
    portENTER_CRITICAL(&schema_mux);
    schema = resolved;
    portEXIT_CRITICAL(&schema_mux);
    return true;
}

static bool decode(const uint8_t *payload, uint16_t len,
                   void *out, uint16_t out_size) {
    if (out_size < sizeof(Sample)) return false;
    Air10Stream::Schema current;
    portENTER_CRITICAL(&schema_mux);
    current = schema;
    portEXIT_CRITICAL(&schema_mux);
    Air10Stream::Frame frame;
    if (!Air10Stream::decode_frame(payload, len, current, frame)) return false;
    auto *sample = static_cast<Sample *>(out);
    if (!Air10Stream::decoded_value(current, frame, "MKP", sample->mkp) ||
        !Air10Stream::decoded_value(current, frame, "RFL", sample->rfl) ||
        !Air10Stream::decoded_value(current, frame, "LYK", sample->lyk)) return false;
    sample->ts_ms = millis();
    return true;
}

void register_parser() {
    LiveStream::register_stream(TAG, decode, SAMPLE_SIZE, prepare);
}

}  // namespace LiveTce

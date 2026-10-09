#pragma once

#include <esp_ota_ops.h>
#include <stddef.h>
#include <stdint.h>

namespace OtaImage {

enum class Encoding : uint8_t { Auto, Plain, Zlib };
const char *encoding_name(Encoding encoding);

struct Status {
    Encoding encoding = Encoding::Auto;
    size_t bytes = 0;
    size_t wire_bytes = 0;
    const char *error = nullptr;
    const char *error_stage = nullptr;
    char partition[17] = {};
};

// Single writer owned by OtaManager's existing exclusive operation lease.
class Writer {
public:
    bool begin(size_t wire_size = 0, size_t image_size = 0,
               Encoding encoding = Encoding::Auto);
    bool write(size_t index, const uint8_t *data, size_t len);
    bool finish();
    void abort();
    const Status &status() const { return status_; }

private:
    bool fail(const char *error, const char *stage = "decode");
    bool resolve_encoding();
    bool decode(const uint8_t *data, size_t len);
    bool write_image(const uint8_t *data, size_t len);

    Status status_;
    Encoding expected_encoding_ = Encoding::Auto;
    size_t wire_size_ = 0;
    size_t image_size_ = 0;
    const esp_partition_t *partition_ = nullptr;
    esp_ota_handle_t handle_ = 0;
    size_t erased_ = 0;
    uint8_t probe_[2] = {};
    size_t probe_size_ = 0;
    void *decoder_ = nullptr;
    uint8_t *dictionary_ = nullptr;
    uint8_t *write_buffer_ = nullptr;
    bool finished_ = false;
};

}  // namespace OtaImage

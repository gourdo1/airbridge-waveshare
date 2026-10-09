#include "ota_image_writer.h"

#include "memory_manager.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <miniz.h>

namespace OtaImage {
namespace {
constexpr size_t WRITE_BYTES = 4096;
constexpr size_t ERASE_BYTES = 64 * 1024;
}

const char *encoding_name(Encoding encoding) {
    switch (encoding) {
    case Encoding::Plain: return "plain";
    case Encoding::Zlib: return "zlib";
    default: return "auto";
    }
}

void Writer::abort() {
    if (handle_) esp_ota_abort(handle_);
    handle_ = 0;
    partition_ = nullptr;
    aircannect::Memory::free(decoder_);
    aircannect::Memory::free(dictionary_);
    heap_caps_free(write_buffer_);
    decoder_ = nullptr;
    dictionary_ = nullptr;
    write_buffer_ = nullptr;
}

bool Writer::fail(const char *error, const char *stage) {
    if (!status_.error) {
        status_.error = error;
        status_.error_stage = stage;
    }
    abort();
    return false;
}

bool Writer::begin(size_t wire_size, size_t image_size, Encoding encoding) {
    abort();
    status_ = {};
    expected_encoding_ = encoding;
    wire_size_ = wire_size;
    image_size_ = image_size;
    probe_size_ = 0;
    finished_ = false;
    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!partition_) return fail("ota_partition_missing", "begin");
    if (image_size > partition_->size) return fail("artifact_too_large", "begin");
    snprintf(status_.partition, sizeof(status_.partition), "%s", partition_->label);

    // IDF otherwise copies PSRAM data through a 32-byte bounce buffer.
    write_buffer_ = static_cast<uint8_t *>(heap_caps_malloc(
        WRITE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!write_buffer_) return fail("ota_buffer_alloc_failed", "begin");
    erased_ = std::min(ERASE_BYTES, size_t(partition_->size));
    esp_err_t err = esp_ota_begin(partition_, erased_, &handle_);
    return err == ESP_OK || fail(esp_err_to_name(err), "begin");
}

bool Writer::resolve_encoding() {
    // Same format detection as AirCANnect's FirmwareInstaller, not filenames.
    if (probe_[0] == 0xe9) {
        status_.encoding = Encoding::Plain;
    } else {
        const uint16_t header = uint16_t(probe_[0]) << 8 | probe_[1];
        if ((probe_[0] & 0x0f) != 8 || (probe_[0] >> 4) > 7 ||
            header % 31 != 0 || (probe_[1] & 0x20))
            return fail("unsupported_ota_image");
        status_.encoding = Encoding::Zlib;
    }
    if (expected_encoding_ != Encoding::Auto &&
        status_.encoding != expected_encoding_)
        return fail("ota_encoding_mismatch");
    if (status_.encoding == Encoding::Plain) {
        if (wire_size_ && image_size_ && wire_size_ != image_size_)
            return fail("upload_size_mismatch");
        return true;
    }

    // Fixed-size decoder and 32 KiB history, also bounded on non-PSRAM boards.
    decoder_ = aircannect::Memory::calloc_large(1, sizeof(tinfl_decompressor));
    dictionary_ = static_cast<uint8_t *>(
        aircannect::Memory::alloc_large(TINFL_LZ_DICT_SIZE));
    if (!decoder_ || !dictionary_) return fail("zlib_alloc_failed");
    tinfl_init(static_cast<tinfl_decompressor *>(decoder_));
    return true;
}

bool Writer::write_image(const uint8_t *data, size_t len) {
    if (!len) return true;
    if (status_.bytes == 0 && data[0] != 0xe9) return fail("bad_esp32_image", "write");
    if (len > partition_->size - status_.bytes ||
        (image_size_ && len > image_size_ - status_.bytes))
        return fail("upload_overrun", "write");

    while (len) {
        const size_t chunk = std::min(WRITE_BYTES, len);
        while (erased_ < status_.bytes + chunk) {
            const size_t erase = std::min(ERASE_BYTES, size_t(partition_->size) - erased_);
            esp_err_t err = esp_partition_erase_range(partition_, erased_, erase);
            if (err != ESP_OK) return fail(esp_err_to_name(err), "erase");
            erased_ += erase;
        }
        memcpy(write_buffer_, data, chunk);
        esp_err_t err = esp_ota_write(handle_, write_buffer_, chunk);
        if (err != ESP_OK) return fail(esp_err_to_name(err), "write");
        status_.bytes += chunk;
        data += chunk;
        len -= chunk;
        yield();
    }
    return true;
}

bool Writer::decode(const uint8_t *data, size_t len) {
    if (!len) return true;
    if (status_.encoding == Encoding::Plain) return write_image(data, len);
    if (finished_) return fail("zlib_trailing_data");

    // Ported from AirCANnect: circular dictionary, Adler32, full output drain.
    size_t remaining = len;
    bool drain = true;
    while (remaining || drain) {
        drain = false;
        size_t consumed = remaining;
        const size_t out_pos = status_.bytes & (TINFL_LZ_DICT_SIZE - 1);
        size_t produced = TINFL_LZ_DICT_SIZE - out_pos;
        // HTTP multipart does not declare the file size. finish() checks EOF.
        const uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_COMPUTE_ADLER32 | TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status result = tinfl_decompress(
            static_cast<tinfl_decompressor *>(decoder_), data, &consumed,
            dictionary_, dictionary_ + out_pos, &produced, flags);
        if (!write_image(dictionary_ + out_pos, produced)) return false;
        data += consumed;
        remaining -= consumed;
        if (result == TINFL_STATUS_DONE) {
            finished_ = true;
            return remaining == 0 || fail("zlib_trailing_data");
        }
        if (result == TINFL_STATUS_NEEDS_MORE_INPUT) {
            if (!remaining) return true;
        } else if (result == TINFL_STATUS_HAS_MORE_OUTPUT) {
            drain = true;
        } else {
            return fail(result == TINFL_STATUS_ADLER32_MISMATCH
                ? "zlib_checksum_failed" : "zlib_decode_failed");
        }
        if (!consumed && !produced) return fail("zlib_decode_stalled");
        yield();
    }
    return true;
}

bool Writer::write(size_t index, const uint8_t *data, size_t len) {
    if (!handle_ || status_.error) return false;
    if (index != status_.wire_bytes || len > SIZE_MAX - index ||
        (wire_size_ && (index > wire_size_ || len > wire_size_ - index)))
        return fail("upload_offset_or_size_mismatch", "input");
    if (!len) return true;
    if (!data) return fail("upload_data_missing", "input");
    const size_t received = len;
    if (status_.encoding == Encoding::Auto) {
        const size_t count = std::min(sizeof(probe_) - probe_size_, len);
        memcpy(probe_ + probe_size_, data, count);
        probe_size_ += count;
        data += count;
        len -= count;
        if (probe_size_ == sizeof(probe_)) {
            if (!resolve_encoding() || !decode(probe_, sizeof(probe_))) return false;
        }
    }
    if (len && !decode(data, len)) return false;
    status_.wire_bytes += received;
    return true;
}

bool Writer::finish() {
    if (!handle_ || status_.error) return false;
    if (status_.encoding == Encoding::Auto || status_.bytes == 0 ||
        (wire_size_ && status_.wire_bytes != wire_size_) ||
        (image_size_ && status_.bytes != image_size_))
        return fail("incomplete_upload", "finish");
    if (status_.encoding == Encoding::Zlib && !finished_)
        return fail("zlib_stream_incomplete", "finish");
    esp_err_t err = esp_ota_end(handle_);
    handle_ = 0;
    if (err != ESP_OK) return fail(esp_err_to_name(err), "validate");
    err = esp_ota_set_boot_partition(partition_);
    if (err != ESP_OK) return fail(esp_err_to_name(err), "boot");
    abort();
    return true;
}

}  // namespace OtaImage

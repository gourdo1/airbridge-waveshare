#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared token output only; each domain owns its JSON layout and traversal.
class JsonCursor {
protected:
    template <typename Next>
    size_t read_tokens(char *out, size_t capacity, Next next) {
        size_t written = 0;
        while (written < capacity) {
            written += drain(out ? out + written : nullptr, capacity - written);
            if (written == capacity || !next()) break;
        }
        return written;
    }

    void token(const char *text, bool quoted = false, size_t length = SIZE_MAX);
    void field(const char *key, const char *text, bool quoted = true);
    void integer(const char *key, int64_t value);
    void decimal(const char *key, int64_t raw, int16_t scale, uint8_t places);
    char number_[48] = {};

private:
    size_t drain(char *out, size_t capacity);
    const char *text_ = nullptr, *pending_ = nullptr;
    size_t offset_ = 0, length_ = 0;
    bool quoted_ = false, pending_quoted_ = false;
    uint8_t quote_phase_ = 0;
    char escape_ = 0;
};

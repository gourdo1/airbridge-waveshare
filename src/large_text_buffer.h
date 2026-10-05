#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class LargeTextBuffer {
public:
    explicit LargeTextBuffer(size_t max_capacity = SIZE_MAX - 1,
                             size_t internal_fallback_limit = SIZE_MAX)
        : max_capacity_(max_capacity), internal_fallback_limit_(internal_fallback_limit) {}
    ~LargeTextBuffer();

    LargeTextBuffer(const LargeTextBuffer &) = delete;
    LargeTextBuffer &operator=(const LargeTextBuffer &) = delete;

    bool reserve(size_t capacity);
    void clear();
    void swap(LargeTextBuffer &other);

    size_t length() const { return length_; }
    size_t capacity() const { return capacity_; }
    bool overflowed() const { return overflowed_; }
    const char *c_str() const { return data_ ? data_ : ""; }

    LargeTextBuffer &operator=(const char *text);
    LargeTextBuffer &operator+=(const char *text);
    LargeTextBuffer &operator+=(char c);

    bool append(const char *text);
    bool append(const char *text, size_t len);

private:
    bool ensure_capacity(size_t needed);

    char *data_ = nullptr;
    size_t length_ = 0;
    size_t capacity_ = 0;
    bool overflowed_ = false;
    size_t max_capacity_, internal_fallback_limit_;
};

}  // namespace aircannect

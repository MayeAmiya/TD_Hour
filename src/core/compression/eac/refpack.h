#pragma once

#include <cstdint>

namespace compression::eac {

struct refpack
{
    static bool is_compressed(const void* data) noexcept;
    // `data_len` / `source_len` / `dest_len` are REQUIRED: these frames come from
    // untrusted archive bytes and declare their own sizes, so the decoder cannot
    // infer any limit on its own.
    static int32_t decoded_size(const void* data, int32_t data_len) noexcept;
    static int32_t decode(void* dest, int32_t dest_len, const void* source,
                          int32_t source_len, int32_t* source_size = nullptr) noexcept;
    static int32_t encode(void* dest, const void* source, int32_t source_size) noexcept;
};

} // namespace compression::eac

#pragma once

#include <cstdint>

namespace compression::eac {

struct btree
{
    static bool is_compressed(const void* data) noexcept;
    // Lengths are REQUIRED: these frames arrive as untrusted archive bytes and
    // declare their own sizes, so the decoder has no other limit available.
    static int32_t decoded_size(const void* data, int32_t data_len) noexcept;
    static int32_t decode(void* dest, int32_t dest_len, const void* source,
                          int32_t source_len, int32_t* source_size = nullptr) noexcept;
    static int32_t encode(void* dest, const void* source, int32_t source_size) noexcept;
};

} // namespace compression::eac

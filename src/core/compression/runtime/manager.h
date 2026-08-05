#pragma once

#include <cstdint>
#include <cstddef>

namespace compression {

enum class type : uint32_t
{
    none = 0,
    refpack,
    noxlzh,  // not implemented, kept for enum completeness
    zlib1, zlib2, zlib3, zlib4, zlib5,
    zlib6, zlib7, zlib8, zlib9,
    btree,
    huffman,
};

struct manager
{
    [[nodiscard]] static bool is_data_compressed(const void* mem, int32_t len) noexcept;
    [[nodiscard]] static type get_type(const void* mem, int32_t len) noexcept;
    [[nodiscard]] static int32_t max_compressed_size(int32_t uncompressed_len, type t) noexcept;
    [[nodiscard]] static int32_t uncompressed_size(const void* mem, int32_t len) noexcept;
    [[nodiscard]] static int32_t compress(type t, const void* src, int32_t src_len, void* dest, int32_t dest_len) noexcept;
    [[nodiscard]] static int32_t decompress(void* dest, int32_t dest_len, const void* src, int32_t src_len) noexcept;

    [[nodiscard]] static const char* name(type t) noexcept;
    [[nodiscard]] static type preferred() noexcept;
    // Returns COMPRESSION_REFPACK by default.
    // RefPack was chosen over zlib because:
    //   • Much faster encode/decode (simple switch-based instruction decoder)
    //   • No external library dependency (pure C, no malloc overhead per call)
    //   • Good compression ratio for TGA images (large solid-color blocks compress well)
    //   • Deterministic CPU usage (no adaptive Huffman / LZMA dictionary building)
    // Trade-off: zlib achieves ~10-20% better compression ratio, but at ~3-5x slower speed.
};

} // namespace compression

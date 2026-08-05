#pragma once

#include <cstdint>
#include <cstring>

// ── Big-endian helpers for EAC codec frame headers ──
// All EAC codecs (RefPack, BTree, Huffman) use big-endian for their frame headers.
// These functions read/write multi-byte values without alignment assumptions.
// Inline + noexcept allows the compiler to optimize away bounds checks.
namespace compression::eac {

inline uint32_t read_be16(const void* src) noexcept
{
    const auto* p = static_cast<const uint8_t*>(src);
    return static_cast<uint32_t>(p[0] << 8) | p[1];
}

inline uint32_t read_be24(const void* src) noexcept
{
    const auto* p = static_cast<const uint8_t*>(src);
    return static_cast<uint32_t>(p[0] << 16) | static_cast<uint32_t>(p[1] << 8) | p[2];
}

inline uint32_t read_be32(const void* src) noexcept
{
    const auto* p = static_cast<const uint8_t*>(src);
    return static_cast<uint32_t>(p[0] << 24) | static_cast<uint32_t>(p[1] << 16) | static_cast<uint32_t>(p[2] << 8) | p[3];
}

inline void write_be16(void* dst, uint32_t val) noexcept
{
    auto* p = static_cast<uint8_t*>(dst);
    p[0] = static_cast<uint8_t>(val >> 8);
    p[1] = static_cast<uint8_t>(val);
}

inline void write_be24(void* dst, uint32_t val) noexcept
{
    auto* p = static_cast<uint8_t*>(dst);
    p[0] = static_cast<uint8_t>(val >> 16);
    p[1] = static_cast<uint8_t>(val >> 8);
    p[2] = static_cast<uint8_t>(val);
}

inline void write_be32(void* dst, uint32_t val) noexcept
{
    auto* p = static_cast<uint8_t*>(dst);
    p[0] = static_cast<uint8_t>(val >> 24);
    p[1] = static_cast<uint8_t>(val >> 16);
    p[2] = static_cast<uint8_t>(val >> 8);
    p[3] = static_cast<uint8_t>(val);
}

} // namespace compression::eac

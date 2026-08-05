#include "manager.h"
#include "eac/refpack.h"
#include "eac/btree.h"
#include "eac/huffman.h"
#include <zlib.h>
#include <cstring>
#include <cmath>

namespace compression {

// The 8-byte manager wrapper stores its size field at byte offset 4 of a buffer
// that comes straight from a memory-mapped archive at an arbitrary file offset,
// so it carries no alignment guarantee.  Reading or writing it through an
// `int32_t*` derived from `uint8_t*` is both a strict-aliasing violation and a
// misaligned access — and the value read is what sizes the decode allocation.
// memcpy is the portable, alignment-agnostic form and compiles to the same load.
namespace {

[[nodiscard]] inline int32_t read_wrapper_size(const uint8_t* p) noexcept
{
    int32_t value = 0;
    std::memcpy(&value, p + 4, sizeof(value));
    return value;
}

inline void write_wrapper_size(uint8_t* d, int32_t value) noexcept
{
    std::memcpy(d + 4, &value, sizeof(value));
}

} // namespace

static const char* s_compression_names[] = {
    "No compression",
    "RefPack",
    "LZHL",
    "ZLib 1 (fast)",  "ZLib 2",  "ZLib 3",  "ZLib 4",
    "ZLib 5 (default)", "ZLib 6", "ZLib 7", "ZLib 8", "ZLib 9 (slow)",
    "BTree",
    "Huff",
};

const char* manager::name(type t) noexcept
{
    auto idx = static_cast<uint32_t>(t);
    if (idx > static_cast<uint32_t>(type::huffman))
        return "Unknown";
    return s_compression_names[idx];
}

type manager::preferred() noexcept
{
    return type::refpack;
}

bool manager::is_data_compressed(const void* mem, int32_t len) noexcept
{
    return get_type(mem, len) != type::none;
}

type manager::get_type(const void* mem, int32_t len) noexcept
{
    if (len < 8)
        return type::none;

    const auto* p = static_cast<const uint8_t*>(mem);

    // ── Fast path: 3-byte magic string comparison ──
    // Each memcmp is ~3 bytes, branch prediction handles the common case (EAR/ZL5) well.
    // TODO: Could be optimized by packing 3-byte magic into uint32_t and using a single
    // switch/case, eliminating 12 separate memcmp calls. Example:
    //   uint32_t tag = p[0] | (p[1]<<8) | (p[2]<<16);
    //   switch(tag) { case 'REA': return type::refpack; ... }
    if (std::memcmp(p, "NOX", 3) == 0) return type::noxlzh;

    if (memcmp(p, "ZL1", 3) == 0) return type::zlib1;
    if (memcmp(p, "ZL2", 3) == 0) return type::zlib2;
    if (memcmp(p, "ZL3", 3) == 0) return type::zlib3;
    if (memcmp(p, "ZL4", 3) == 0) return type::zlib4;
    if (memcmp(p, "ZL5", 3) == 0) return type::zlib5;
    if (memcmp(p, "ZL6", 3) == 0) return type::zlib6;
    if (memcmp(p, "ZL7", 3) == 0) return type::zlib7;
    if (memcmp(p, "ZL8", 3) == 0) return type::zlib8;
    if (memcmp(p, "ZL9", 3) == 0) return type::zlib9;

    if (memcmp(p, "EAB", 3) == 0) return type::btree;
    if (memcmp(p, "EAH", 3) == 0) return type::huffman;
    if (memcmp(p, "EAR", 3) == 0) return type::refpack;

    // ── Fallback: detect by raw header bytes (no magic string prefix) ──
    // This handles files that were compressed directly by the EAC codecs
    // without the manager's 8-byte wrapper header.
    if (eac::refpack::is_compressed(mem)) return type::refpack;
    if (eac::btree::is_compressed(mem)) return type::btree;
    if (eac::huffman::is_compressed(mem)) return type::huffman;

    return type::none;
}

int32_t manager::max_compressed_size(int32_t uncompressed_len, type t) noexcept
{
    switch (t)
    {
    case type::noxlzh:
        return uncompressed_len + 8;
    case type::btree:
    case type::huffman:
    case type::refpack:
        return uncompressed_len + 8;
    case type::zlib1:
    case type::zlib2:
    case type::zlib3:
    case type::zlib4:
    case type::zlib5:
    case type::zlib6:
    case type::zlib7:
    case type::zlib8:
    case type::zlib9:
        return static_cast<int32_t>(std::ceil(uncompressed_len * 1.1 + 12 + 8));
    default:
        return 0;
    }
}

int32_t manager::uncompressed_size(const void* mem, int32_t len) noexcept
{
    if (len < 8) return len;

    auto t = get_type(mem, len);
    if (t == type::none) return len;

    const auto* p = static_cast<const uint8_t*>(mem);

    // Check if this has the manager's 8-byte wrapper ("EAR\0" + size)
    bool hasWrapper = (std::memcmp(p, "EAR", 3) == 0)
                   || (std::memcmp(p, "EAB", 3) == 0)
                   || (std::memcmp(p, "EAH", 3) == 0);

    if (hasWrapper)
    {
        // Manager-wrapped: uncompressed size is at p[4..7]
        return read_wrapper_size(p);
    }

    // Raw codec frames (no wrapper): use codec-specific size extraction
    if (t == type::refpack) return eac::refpack::decoded_size(p, len);
    if (t == type::btree) return eac::btree::decoded_size(p, len);
    if (t == type::huffman) return eac::huffman::decoded_size(p, len);

    return read_wrapper_size(p);
}

int32_t manager::compress(type t, const void* src, int32_t src_len, void* dest, int32_t dest_len) noexcept
{
    if (dest_len < 8) return 0;
    dest_len -= 8;

    auto* s = static_cast<const uint8_t*>(src);
    auto* d = static_cast<uint8_t*>(dest);

    // ── Format: [4-byte magic] [4-byte uncompressed size] [compressed payload] ──
    // The magic is written by manager layer; each codec writes its own frame header
    // starting at dest+8. For example, RefPack writes 0x10FB at offset 0 (relative to d+8),
    // so the full on-disk layout is:
    //   d[0..3]   = "EAR\0"
    //   d[4..7]   = uncompressed size (little-endian int32)
    //   d[8..9]   = 0x10FB (RefPack frame type)
    //   d[10..]   = compressed data stream

    if (t == type::btree)
    {
        std::memcpy(d, "EAB\0", 4);
        write_wrapper_size(d, 0);
        auto ret = eac::btree::encode(d + 8, s, src_len);
        if (ret)
        {
            write_wrapper_size(d, src_len);
            return ret + 8;
        }
        return 0;
    }

    if (t == type::huffman)
    {
        std::memcpy(d, "EAH\0", 4);
        write_wrapper_size(d, 0);
        auto ret = eac::huffman::encode(d + 8, s, src_len);
        if (ret)
        {
            write_wrapper_size(d, src_len);
            return ret + 8;
        }
        return 0;
    }

    if (t == type::refpack)
    {
        std::memcpy(d, "EAR\0", 4);
        write_wrapper_size(d, 0);
        auto ret = eac::refpack::encode(d + 8, s, src_len);
        if (ret)
        {
            write_wrapper_size(d, src_len);
            return ret + 8;
        }
        return 0;
    }

    if (t >= type::zlib1 && t <= type::zlib9)
    {
        auto level = static_cast<int32_t>(t) - static_cast<int32_t>(type::zlib1) + 1;
        std::memcpy(d, "ZL0\0", 4);
        d[2] = '0' + static_cast<uint8_t>(level);
        write_wrapper_size(d, 0);

        uLong out_len = static_cast<uLong>(dest_len);
        auto err = compress2(d + 8, &out_len, s, static_cast<uLong>(src_len), level);

        if (err == Z_OK || err == Z_STREAM_END)
        {
            write_wrapper_size(d, src_len);
            return static_cast<int32_t>(out_len) + 8;
        }
        return 0;
    }

    return 0;
}

int32_t manager::decompress(void* dest, int32_t dest_len, const void* src, int32_t src_len) noexcept
{
    if (src_len < 8) return 0;

    auto* s = static_cast<const uint8_t*>(src);
    auto* d = static_cast<uint8_t*>(dest);

    // ── Decompression flow ──
    // 1. Detect compression type by magic string at src[0..2]
    // 2. Uncompressed size is at src[4..7] (little-endian int32)
    // 3. Compressed payload starts at src[8]
    // 4. Each codec decodes its own frame header (e.g. RefPack reads 0x10FB at offset 0)
    // 5. Returns decoded size, or 0 on failure
    auto t = get_type(src, src_len);

    if (t == type::btree)
    {
        const uint8_t* frameStart = s;
        int32_t frameLen = src_len;
        if (src_len >= 8 && std::memcmp(s, "EAB", 3) == 0)
        {
            frameStart = s + 8;
            frameLen = src_len - 8;
        }
        auto ret = eac::btree::decode(d, dest_len, frameStart, frameLen, &frameLen);
        return ret ? ret : 0;
    }

    if (t == type::huffman)
    {
        const uint8_t* frameStart = s;
        int32_t frameLen = src_len;
        if (src_len >= 8 && std::memcmp(s, "EAH", 3) == 0)
        {
            frameStart = s + 8;
            frameLen = src_len - 8;
        }
        auto ret = eac::huffman::decode(d, dest_len, frameStart, frameLen, &frameLen);
        return ret ? ret : 0;
    }

    if (t == type::refpack)
    {
        // Check if this has the manager's 8-byte wrapper ("EAR\0" + size)
        const uint8_t* frameStart = s;
        int32_t frameLen = src_len;
        if (src_len >= 8 && std::memcmp(s, "EAR", 3) == 0)
        {
            frameStart = s + 8;
            frameLen = src_len - 8;
        }
        auto ret = eac::refpack::decode(d, dest_len, frameStart, frameLen, &frameLen);
        return ret ? ret : 0;
    }

    if (t == type::noxlzh)
    {
        // Nox LZH not implemented
        return 0;
    }

    if (t >= type::zlib1 && t <= type::zlib9)
    {
        uLong out_len = static_cast<uLong>(dest_len);
        auto err = uncompress(d, &out_len, s + 8, static_cast<uLong>(src_len - 8));
        if (err == Z_OK || err == Z_STREAM_END)
            return static_cast<int32_t>(out_len);
        return 0;
    }

    return 0;
}

} // namespace compression

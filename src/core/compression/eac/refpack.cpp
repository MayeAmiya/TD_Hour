#include "refpack.h"
#include "codec_common.h"
#include <cstring>
#include <cstdlib>
#include <new>

// ── RefPack (Electronic Arts Compression) ──
// LZ77-based dictionary compression used by EA games (Medal of Honor, Star Wars Racer, etc.)
//
// Frame format:
//   Short frame (≤16 MB):  2-byte magic (0x10FB) + 3-byte uncompressed size + compressed data
//   Long frame (>16 MB):   2-byte magic (0x90FB) + 4-byte uncompressed size + compressed data
//
// Instruction set (variable-length, 1-4 bytes per instruction):
//   Literal block:  0xE0 + (count>>2) - 1  →  followed by 1..112 raw bytes
//   Short ref:      bit7=0, bit6=0         →  2-byte offset + length 3..10
//   Int ref:        bit7=0, bit6=1         →  3-byte offset + length 4..67
//   Very-int ref:   bit7=1, bit6=0         →  4-byte offset + length 5..1028
//   EOF:            bit7=1, bit6=1, bit5=1 →  0xFC + 0..3 trailing literals
//
// Each instruction optionally precedes 0..3 literal bytes before the reference.
// Maximum backward reference distance: 131,071 bytes (fits in 17 bits).

namespace compression::eac {

bool refpack::is_compressed(const void* data) noexcept
{
    auto type = read_be16(data);
    return type == 0x10fb || type == 0x11fb || type == 0x90fb || type == 0x91fb;
}

int32_t refpack::decoded_size(const void* data, int32_t data_len) noexcept
{
    // The header can extend to byte 10, but callers only guarantee 8 bytes, so
    // every field read is bounded here rather than trusted.
    if (!data || data_len < 2) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    const auto readable = [data_len](int32_t offset, int32_t width) noexcept {
        return offset >= 0 && width > 0 && data_len - offset >= width;
    };
    auto type = read_be16(data);
    if (type & 0x8000)
    {
        const int32_t offset = (type & 0x100) ? 6 : 2;
        if (!readable(offset, 4)) return 0;
        return static_cast<int32_t>(read_be32(p + offset));
    }
    const int32_t offset = (type & 0x100) ? 5 : 2;
    if (!readable(offset, 3)) return 0;
    return static_cast<int32_t>(read_be24(p + offset));
}

int32_t refpack::decode(void* dest, int32_t dest_len, const void* source,
                        int32_t source_len, int32_t* source_size) noexcept
{
    auto* s = static_cast<const uint8_t*>(source);
    auto* d = static_cast<uint8_t*>(dest);
    int32_t ulen = 0;

    if (!s || !d || source_len <= 0 || dest_len < 0) return 0;

    // ── Bounds contract ──
    // This decoder is fed bytes straight from a memory-mapped archive, and the
    // declared output size comes from the SAME untrusted frame, so nothing
    // correlates the instruction stream with either buffer.  Every source read
    // and every destination write is therefore bounded here, and a back-reference
    // is rejected if it would point before the start of the output.  Previously
    // this function had, by its own comment, "no bounds check on d" and no check
    // on s at all: a crafted or corrupt .big entry could read before and write
    // after the heap buffer the caller allocated.
    const uint8_t* const sourceBegin = s;
    const uint8_t* const sourceEnd = s + source_len;
    uint8_t* const destBegin = d;
    uint8_t* const destEnd = d + dest_len;

    const auto sourceAvailable = [&](uint32_t count) noexcept {
        return static_cast<uint64_t>(sourceEnd - s) >= count;
    };
    const auto destAvailable = [&](uint32_t count) noexcept {
        return static_cast<uint64_t>(destEnd - d) >= count;
    };
    // `while (run--)` performs `run` copies.
    const auto copyLiterals = [&](uint32_t count) noexcept {
        if (!sourceAvailable(count) || !destAvailable(count)) return false;
        // memmove, not memcpy: this decoder supports in-place decompression, so
        // the destination can sit inside the source region.  For the forward
        // d < s case memmove gives the same bytes as the original per-byte loop.
        if (count != 0) std::memmove(d, s, count);
        d += count;
        s += count;
        return true;
    };
    // Back-references may overlap their own output, so copy byte-by-byte.
    // `do { } while (run--)` performs run + 1 copies; callers pass the real count.
    const auto copyReference = [&](uint32_t offset, uint32_t count) noexcept {
        const uint64_t produced = static_cast<uint64_t>(d - destBegin);
        if (static_cast<uint64_t>(offset) + 1u > produced) return false;
        if (!destAvailable(count)) return false;
        const uint8_t* ref = d - 1 - offset;
        for (uint32_t index = 0; index < count; ++index) *d++ = *ref++;
        return true;
    };

    if (!sourceAvailable(2)) return 0;
    uint32_t type = read_be16(s);
    s += 2;

    if (type & 0x8000)
    {
        if (type & 0x100)
        {
            if (!sourceAvailable(4)) return 0;
            s += 4;
        }
        if (!sourceAvailable(4)) return 0;
        ulen = static_cast<int32_t>(read_be32(s));
        s += 4;
    }
    else
    {
        if (type & 0x100)
        {
            if (!sourceAvailable(3)) return 0;
            s += 3;
        }
        if (!sourceAvailable(3)) return 0;
        ulen = static_cast<int32_t>(read_be24(s));
        s += 3;
    }

    // The frame's own declared size must fit the buffer the caller sized from
    // decoded_size(); anything larger is malformed rather than merely truncated.
    if (ulen < 0 || ulen > dest_len) return 0;

    // ── Decode loop ──
    // Each instruction either copies N literal bytes to the output or copies N
    // bytes from a backward reference into already-decoded output.
    for (;;)
    {
        if (!sourceAvailable(1)) return 0;
        uint8_t first = *s++;
        // Short form reference: bit7=0. Offset ≤ 1024, length 3..10.
        if (!(first & 0x80))
        {
            if (!sourceAvailable(1)) return 0;
            uint8_t second = *s++;
            if (!copyLiterals(first & 3u)) return 0;
            const uint32_t offset = ((first & 0x60u) << 3) + second;
            if (!copyReference(offset, ((first & 0x1cu) >> 2) + 3u)) return 0;
            continue;
        }
        if (!(first & 0x40))
        {
            // Int form reference: bit7=1, bit6=0. Offset ≤ 16384, length 4..67.
            if (!sourceAvailable(2)) return 0;
            uint8_t second = *s++;
            uint8_t third = *s++;
            if (!copyLiterals(static_cast<uint32_t>(second) >> 6)) return 0;
            const uint32_t offset = ((second & 0x3fu) << 8) + third;
            if (!copyReference(offset, (first & 0x3fu) + 4u)) return 0;
            continue;
        }
        if (!(first & 0x20))
        {
            // Very-int form reference. Offset ≤ 131071, length 5..1028.
            if (!sourceAvailable(3)) return 0;
            uint8_t second = *s++;
            uint8_t third = *s++;
            uint8_t forth = *s++;
            if (!copyLiterals(first & 3u)) return 0;
            const uint32_t offset = (((first & 0x10u) >> 4) << 16) +
                (static_cast<uint32_t>(second) << 8) + third;
            if (!copyReference(offset, (((first & 0x0cu) >> 2) << 8) + forth + 5u))
                return 0;
            continue;
        }
        // Literal block: bit7=1, bit6=1, bit5=1. Emit 4..112 literal bytes.
        uint32_t run = ((first & 0x1fu) << 2) + 4;
        if (run <= 112)
        {
            if (!copyLiterals(run)) return 0;
            continue;
        }
        // EOF marker: low 2 bits are 0..3 trailing literals before the end.
        if (!copyLiterals(first & 3u)) return 0;
        break;
    }

    if (source_size)
        *source_size = static_cast<int32_t>(s - sourceBegin);
    return ulen;
}

static uint32_t match_len(const uint8_t* s, const uint8_t* d, uint32_t max_match)
{
    uint32_t cur;
    for (cur = 0; cur < max_match && *s++ == *d++; ++cur)
        ;
    return cur;
}

#define HASH(cptr) (int32_t)((((uint32_t)(uint8_t)(cptr)[0] << 8) | ((uint32_t)(uint8_t)(cptr)[2])) ^ ((uint32_t)(uint8_t)(cptr)[1] << 4))

static int32_t ref_compress(const uint8_t* from, int32_t len, uint8_t* dest, int32_t max_back, int32_t quick)
{
    uint8_t* to = dest;
    uint32_t run = 0;
    const uint8_t* cptr = from;
    const uint8_t* rptr = from;
    int32_t remaining = len;

    if (max_back > 131071) max_back = 131071;

    // ── Hash-chain LZ77 encoder ──
    //
    // Uses a 65536-entry hash table + 131072-entry link chain to find the longest
    // matching substring within the last 131071 bytes.
    //
    // Hash: HASH(p) = ((p[0]<<8) | p[2]) ^ (p[1]<<4)
    // This uses bytes 0 and 2 for the hash key, XORed with byte 1 shifted — a
    // simple triplet hash that distributes well for image/text data.
    //
    // ── PERFORMANCE NOTE ──
    // hashtbl (256 KB) + link (512 KB) = 768 KB allocated on EVERY compress call.
    // This is the single biggest bottleneck: malloc + memset(256KB) per compression.
    //
    // For typical TGA files (50-500 KB), this allocation overhead dominates runtime.
    //
    // ── Optimization candidate: thread-local cache ──
    // Replace malloc/free with `static thread_local` arrays to eliminate:
    //   • 768 KB heap allocation per call
    //   • 256 KB memset(-1) per call
    //   • Cache misses from fresh allocations
    //
    // Expected speedup: 20-40% for small-to-medium inputs (<1 MB), where allocation
    // overhead is a significant fraction of total time. Thread_local is safe because
    // each thread gets its own buffer, and compression is typically single-threaded
    // per asset (textures loaded on main thread).
    //
    // ── Encoding strategy ──
    // For each input position, try to find the longest match in the sliding window.
    // Choose the best instruction format based on offset distance and match length:
    //   • offset < 1024  && len ≤ 10  → 2-byte short form (fastest)
    //   • offset < 16384 && len ≤ 67  → 3-byte int form
    //   • otherwise                                       → 4-byte very-int form
    // If no match is better than literal copy, emit the byte as a literal.
    // Accumulate consecutive literals into blocks of up to 112 bytes (0xE0 instruction).
    auto* hashtbl = static_cast<int32_t*>(std::malloc(65536 * sizeof(int32_t)));
    auto* link = static_cast<int32_t*>(std::malloc(131072 * sizeof(int32_t)));
    if (!hashtbl || !link)
    {
        std::free(hashtbl);
        std::free(link);
        return 0;
    }

    std::memset(hashtbl, -1, 65536 * sizeof(int32_t));
    remaining -= 4;

    while (remaining >= 0)
    {
        uint32_t boffset = 0;
        uint32_t blen = 2;
        uint32_t bcost = 2;
        uint32_t mlen = remaining < 1028 ? static_cast<uint32_t>(remaining) : 1028;
        const uint8_t* tptr = cptr - 1;
        int32_t hash = HASH(cptr);
        int32_t hoffset = hashtbl[hash];
        int32_t min_hoffset = static_cast<int32_t>((cptr - from) - 131071);
        if (min_hoffset < 0) min_hoffset = 0;

        if (hoffset >= min_hoffset)
        {
            do
            {
                tptr = from + hoffset;
                if (cptr[blen] == tptr[blen])
                {
                    uint32_t tlen = match_len(cptr, tptr, mlen);
                    if (tlen > blen)
                    {
                        uint32_t toffset = static_cast<uint32_t>((cptr - 1) - tptr);
                        uint32_t tcost = toffset < 1024 && tlen <= 10 ? 2
                            : toffset < 16384 && tlen <= 67 ? 3 : 4;

                        if (tlen - tcost + 4 > blen - bcost + 4)
                        {
                            blen = tlen;
                            bcost = tcost;
                            boffset = toffset;
                            if (blen >= 1028) break;
                        }
                    }
                }
            } while ((hoffset = link[hoffset & 131071]) >= min_hoffset);
        }

        if (bcost >= blen || remaining < 4)
        {
            hoffset = static_cast<int32_t>(cptr - from);
            link[hoffset & 131071] = hashtbl[hash];
            hashtbl[hash] = hoffset;
            ++run;
            ++cptr;
            --remaining;
        }
        else
        {
            while (run > 3)
            {
                uint32_t tlen = run & ~3;
                if (tlen > 112) tlen = 112;
                run -= tlen;
                *to++ = static_cast<uint8_t>(0xe0 + (tlen >> 2) - 1);
                std::memcpy(to, rptr, tlen);
                rptr += tlen;
                to += tlen;
            }

            if (bcost == 2)
            {
                *to++ = static_cast<uint8_t>(((boffset >> 8) << 5) + ((blen - 3) << 2) + run);
                *to++ = static_cast<uint8_t>(boffset);
            }
            else if (bcost == 3)
            {
                *to++ = static_cast<uint8_t>(0x80 + (blen - 4));
                *to++ = static_cast<uint8_t>((run << 6) + (boffset >> 8));
                *to++ = static_cast<uint8_t>(boffset);
            }
            else
            {
                *to++ = static_cast<uint8_t>(0xc0 + ((boffset >> 16) << 4) + (((blen - 5) >> 8) << 2) + run);
                *to++ = static_cast<uint8_t>(boffset >> 8);
                *to++ = static_cast<uint8_t>(boffset);
                *to++ = static_cast<uint8_t>(blen - 5);
            }

            if (run)
            {
                std::memcpy(to, rptr, run);
                to += run;
                run = 0;
            }

            if (quick)
            {
                hoffset = static_cast<int32_t>(cptr - from);
                link[hoffset & 131071] = hashtbl[hash];
                hashtbl[hash] = hoffset;
                cptr += blen;
            }
            else
            {
                for (uint32_t i = 0; i < blen; ++i)
                {
                    hash = HASH(cptr);
                    hoffset = static_cast<int32_t>(cptr - from);
                    link[hoffset & 131071] = hashtbl[hash];
                    hashtbl[hash] = hoffset;
                    ++cptr;
                }
            }

            rptr = cptr;
            remaining -= static_cast<int32_t>(blen);
        }
    }

    remaining += 4;
    run += remaining;
    while (run > 3)
    {
        uint32_t tlen = run & ~3;
        if (tlen > 112) tlen = 112;
        run -= tlen;
        *to++ = static_cast<uint8_t>(0xe0 + (tlen >> 2) - 1);
        std::memcpy(to, rptr, tlen);
        rptr += tlen;
        to += tlen;
    }

    *to++ = static_cast<uint8_t>(0xfc + run);
    if (run)
    {
        std::memcpy(to, rptr, run);
        to += run;
    }

    std::free(link);
    std::free(hashtbl);
    return static_cast<int32_t>(to - dest);
}

// ── Encode entry point ──
// Wraps ref_compress() with the appropriate frame header (short or long).
// Returns total compressed size including the RefPack frame header (5 or 6 bytes).
// Does NOT include the manager's 8-byte wrapper (4-byte magic + 4-byte size).
int32_t refpack::encode(void* dest, const void* source, int32_t source_size) noexcept
{
    if (source_size > 0xffffff)
    {
        write_be16(dest, 0x90fb);
        write_be32(static_cast<uint8_t*>(dest) + 2, static_cast<uint32_t>(source_size));
        return 6 + ref_compress(static_cast<const uint8_t*>(source), source_size,
            static_cast<uint8_t*>(dest) + 6, 131072, 0);
    }
    else
    {
        write_be16(dest, 0x10fb);
        write_be24(static_cast<uint8_t*>(dest) + 2, static_cast<uint32_t>(source_size));
        return 5 + ref_compress(static_cast<const uint8_t*>(source), source_size,
            static_cast<uint8_t*>(dest) + 5, 131072, 0);
    }
}

} // namespace compression::eac

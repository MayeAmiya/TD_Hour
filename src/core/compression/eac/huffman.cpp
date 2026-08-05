#include "huffman.h"
#include "codec_common.h"
#include <cstring>
#include <cstdlib>
#include <new>

// ── Huffman (Electronic Arts Compression) ──
// Variable-length entropy coder with 12 supported frame types (0x30FB-0x35FB, 0xB0FB-0xB5FB).
// The decoder reconstructs a Huffman tree from the compressed stream and decodes
// symbol-by-symbol using bit-packing.
//
// Frame types 0x32FB/0xB2FB and 0x34FB/0xB4FB include post-processing:
//   0x32/0xB2: cumulative sum (integral image style)
//   0x34/0xB4: cumulative sum of cumulative sums
//
// ── Note ──
// encode() is stubbed out (returns 0). The full encoder from the original
// huffencode.cpp (~700 lines) is not needed for game data compatibility —
// we only need to decode pre-compressed assets. Use refpack or zlib for new compression.
#pragma warning(push)
#pragma warning(disable : 4293) // shift count >= width of type (guarded by runtime check)

namespace compression::eac {

// ── Decode ─────────────────────────────────────────────────────────────────

struct huff_decode_ctx
{
    const uint8_t* s{};
    int32_t bitsleft{};
    uint32_t bits{};
};

// Bounds contract for the decoder below.  Both cursors walk untrusted archive
// bytes whose declared output size comes from the same frame, so neither can be
// trusted.  `qs` only ever advances here and `qd` only ever advances through
// HUF_PUT, so bounding these two macros bounds the whole decoder.
//
// A source underrun must NOT hard-fail: the bit reader prefetches 16 bits at a
// time, so a well-formed frame can legitimately ask for one more word after its
// last symbol (the original code satisfied that by reading harmless bytes past
// the payload).  Feed zero bits instead and only give up once the padding grows
// past anything a real tail could need — that keeps valid frames decoding while
// still refusing to read outside the buffer or spin forever.
#define HUF_MAX_ZERO_REFILLS 8

#define HUF_GET16BITS() \
    do { \
        if (static_cast<uint64_t>(qsEnd - qs) < 2u) { \
            if (++huffZeroRefills > HUF_MAX_ZERO_REFILLS) { \
                huffOverflow = true; \
                goto huff_done; \
            } \
            bitsunshifted = bitsunshifted << 16; \
        } else { \
            bitsunshifted = static_cast<uint32_t>(qs[0]) | (bitsunshifted << 8); \
            bitsunshifted = static_cast<uint32_t>(qs[1]) | (bitsunshifted << 8); \
            qs += 2; \
        } \
    } while (0)

#define HUF_PUT(value) \
    do { \
        if (qd >= qdEnd) { huffOverflow = true; goto huff_done; } \
        *qd++ = (value); \
    } while (0)

#define HUF_GETBITS(v, n) \
    do { \
        if (n) { \
            v = bits >> (32 - (n)); \
            bits <<= (n); \
            bitsleft -= (n); \
        } \
        if (bitsleft < 0) { \
            HUF_GET16BITS(); \
            bits = bitsunshifted << (-bitsleft); \
            bitsleft += 16; \
        } \
    } while (0)

#define HUF_GETNUM(v) \
    do { \
        if (static_cast<int32_t>(bits) < 0) { \
            HUF_GETBITS(v, 3); \
            v -= 4; \
        } else { \
            int32_t n_; \
            uint32_t v1_; \
            if (bits >> 16) { \
                n_ = 2; \
                do { bits <<= 1; ++n_; } while (static_cast<int32_t>(bits) >= 0); \
                bits <<= 1; bitsleft -= (n_ - 1); \
                HUF_GETBITS(v, 0); \
            } else { \
                n_ = 2; \
                do { ++n_; HUF_GETBITS(v, 1); } while (!v); \
            } \
            if (n_ > 16) { \
                HUF_GETBITS(v, n_ - 16); \
                HUF_GETBITS(v1_, 16); \
                v = (v1_ | (v << 16)) + (static_cast<uint32_t>(1) << n_) - 4; \
            } else { \
                HUF_GETBITS(v, n_); \
                v = v + (static_cast<uint32_t>(1) << n_) - 4; \
            } \
        } \
    } while (0)

#define HUF_MEMSET(ptr, val, amt) \
    do { \
        auto _p = static_cast<uint32_t*>((void*)(ptr)); \
        auto _v = static_cast<uint32_t>((val) << 24 | (val) << 16 | (val) << 8 | (val)); \
        auto _n = (amt) / 16; \
        while (_n--) { *_p++ = _v; *_p++ = _v; *_p++ = _v; *_p++ = _v; } \
    } while (0)

static int32_t huff_decompress(const uint8_t* pack, int32_t pack_len,
                               uint8_t* unpack, int32_t unpack_len)
{
    auto* qs = const_cast<uint8_t*>(pack);
    auto* qd = unpack;
    int32_t ulen = 0;

    if (!qs || !unpack || pack_len <= 0 || unpack_len < 0) return 0;

    const uint8_t* const qsEnd = qs + pack_len;
    uint8_t* const qdEnd = unpack + unpack_len;
    bool huffOverflow = false;
    int32_t huffZeroRefills = 0;

    int32_t mostbits;
    uint8_t clue{};
    int32_t cluelen = 0;
    uint32_t bits{};
    uint32_t bitsunshifted{};
    int32_t bitnumtbl[16]{};
    uint32_t deltatbl[16]{};
    uint32_t cmptbl[16]{};
    uint8_t codetbl[256]{};
    uint8_t quickcodetbl[256]{};
    uint8_t quicklentbl[256];

    // Declared before the first HUF_GETBITS: those macros can `goto huff_done`,
    // and jumping over a variable's initialization is ill-formed (MSVC only
    // permits it as an extension).  `type` in particular is read at huff_done.
    uint32_t v = 0;
    uint32_t type = 0;

    auto bitsleft = -16;
    bits = 0;
    HUF_GETBITS(bits, 0);

    HUF_GETBITS(v, 16);
    type = v;

    if (type & 0x8000)
    {
        if (type & 0x100)
        {
            HUF_GETBITS(v, 16);
            HUF_GETBITS(v, 16);
        }
        type &= ~0x100;
        HUF_GETBITS(v, 16);
        HUF_GETBITS(ulen, 16);
        ulen |= static_cast<int32_t>(v << 16);
    }
    else
    {
        if (type & 0x100)
        {
            HUF_GETBITS(v, 8);
            HUF_GETBITS(v, 16);
        }
        type &= ~0x100;
        HUF_GETBITS(v, 8);
        HUF_GETBITS(ulen, 16);
        ulen |= static_cast<int32_t>(v << 16);
    }

    {
        uint32_t t;
        HUF_GETBITS(t, 8);
        clue = static_cast<uint8_t>(t);

        int32_t numchars = 0;
        int32_t numbits = 1;
        uint32_t basecmp = 0;

        do
        {
            basecmp <<= 1;
            deltatbl[numbits] = basecmp - static_cast<uint32_t>(numchars);

            int32_t bitnum;
            HUF_GETNUM(bitnum);
            bitnumtbl[numbits] = bitnum;

            numchars += bitnum;
            basecmp += static_cast<uint32_t>(bitnum);

            uint32_t cmp = 0;
            if (bitnum)
                cmp = (basecmp << (16 - numbits)) & 0xffff;

            cmptbl[numbits++] = cmp;
        } while (!bitnumtbl[numbits - 1] || cmptbl[numbits - 1]);

        cmptbl[numbits - 1] = 0xffffffff;
        mostbits = numbits - 1;

        {
            int8_t leap[256];
            uint8_t nextchar = static_cast<uint8_t>(-1);
            HUF_MEMSET(leap, 0, 256);

            for (int32_t i = 0; i < numchars; ++i)
            {
                int32_t leapdelta;
                HUF_GETNUM(leapdelta);
                ++leapdelta;

                do
                {
                    ++nextchar;
                    if (!leap[nextchar]) --leapdelta;
                } while (leapdelta);

                leap[nextchar] = 1;
                codetbl[i] = nextchar;
            }
        }
    }

    HUF_MEMSET(quicklentbl, 64, 256);

    {
        auto* codeptr = codetbl;
        auto* quickcodeptr = quickcodetbl;
        auto* quicklenptr = quicklentbl;

        for (int32_t bits = 1; bits <= mostbits; ++bits)
        {
            auto bitnum = bitnumtbl[bits];
            if (bits >= 9) break;
            auto numbitentries = 1 << (8 - bits);

            while (bitnum--)
            {
                auto nextcode = *codeptr++;
                auto nextlen = bits;
                if (nextcode == clue)
                {
                    cluelen = bits;
                    nextlen = 96;
                }
                for (int32_t i = 0; i < numbitentries; ++i)
                {
                    *quickcodeptr++ = static_cast<uint8_t>(nextcode);
                    *quicklenptr++ = static_cast<uint8_t>(nextlen);
                }
            }
        }
    }

    for (;;)
    {
        auto* quickcodeptr = quickcodetbl;
        auto* quicklenptr = quicklentbl;

        int32_t numbits;
        uint32_t cmp;

        goto nextloop;

        do
        {
            HUF_PUT(quickcodeptr[bits >> 24]);
            HUF_GET16BITS();
            bits = bitsunshifted << (16 - bitsleft);

nextloop:
            numbits = quicklenptr[bits >> 24];
            bitsleft -= numbits;

            if (bitsleft >= 0)
            {
                do
                {
                    HUF_PUT(quickcodeptr[bits >> 24]);
                    bits <<= numbits;
                    numbits = quicklenptr[bits >> 24];
                    bitsleft -= numbits;
                    if (bitsleft < 0) break;
                    HUF_PUT(quickcodeptr[bits >> 24]);
                    bits <<= numbits;
                    numbits = quicklenptr[bits >> 24];
                    bitsleft -= numbits;
                    if (bitsleft < 0) break;
                    HUF_PUT(quickcodeptr[bits >> 24]);
                    bits <<= numbits;
                    numbits = quicklenptr[bits >> 24];
                    bitsleft -= numbits;
                    if (bitsleft < 0) break;
                    HUF_PUT(quickcodeptr[bits >> 24]);
                    bits <<= numbits;
                    numbits = quicklenptr[bits >> 24];
                    bitsleft -= numbits;
                } while (bitsleft >= 0);
            }
            bitsleft += 16;
        } while (bitsleft >= 0);

        bitsleft = bitsleft - 16 + numbits;

        {
            uint8_t code;

            if (numbits != 96)
            {
                cmp = bits >> 16;
                numbits = 8;
                do { ++numbits; } while (cmp >= cmptbl[numbits]);
            }
            else
                numbits = cluelen;

            cmp = bits >> (32 - numbits);
            bits <<= numbits;
            bitsleft -= numbits;

            code = codetbl[cmp - deltatbl[numbits]];

            if (code != clue && bitsleft >= 0)
            {
                HUF_PUT(code);
                goto nextloop;
            }

            if (bitsleft < 0)
            {
                HUF_GET16BITS();
                bits = bitsunshifted << -bitsleft;
                bitsleft += 16;
            }

            if (code != clue)
            {
                HUF_PUT(code);
                goto nextloop;
            }

            {
                int32_t runlen = 0;
                HUF_GETNUM(runlen);
                if (runlen)
                {
                    // `runlen` is an unbounded HUF_GETNUM value and the run
                    // repeats the previously emitted byte, so this needs both a
                    // "something was emitted" check (the old code read *(qd - 1)
                    // even when qd was still at the start of the buffer) and a
                    // destination bound (dest2 = qd + runlen could land anywhere).
                    if (runlen < 0 || qd == unpack) { huffOverflow = true; goto huff_done; }
                    if (static_cast<uint64_t>(qdEnd - qd) < static_cast<uint64_t>(runlen))
                    {
                        huffOverflow = true;
                        goto huff_done;
                    }
                    auto* d = qd;
                    auto* dest2 = d + runlen;
                    code = *(d - 1);
                    do { *d++ = code; } while (d < dest2);
                    qd = d;
                    goto nextloop;
                }
            }

            HUF_GETBITS(v, 1);
            if (v) break;

            {
                uint32_t t;
                HUF_GETBITS(t, 8);
                code = static_cast<uint8_t>(t);
            }
            HUF_PUT(code);
            goto nextloop;
        }
    }

huff_done:
    // Any bound hit means the frame was malformed or truncated: report failure
    // rather than handing the caller a partially written buffer and a length that
    // does not describe it.
    if (huffOverflow) return 0;

    // The post-processing passes below walk `ulen` bytes of the destination, so
    // the declared length must fit the buffer the caller actually provided.
    if (ulen < 0 || ulen > unpack_len) return 0;

    if (type == 0x32fb || type == 0xb2fb)
    {
        int32_t i = 0;
        qd = unpack;
        while (qd < unpack + ulen)
        {
            i += static_cast<int32_t>(*qd);
            *qd++ = static_cast<uint8_t>(i);
        }
    }
    else if (type == 0x34fb || type == 0xb4fb)
    {
        int32_t i = 0;
        int32_t nextchar = 0;
        qd = unpack;
        while (qd < unpack + ulen)
        {
            i += static_cast<int32_t>(*qd);
            nextchar += i;
            *qd++ = static_cast<uint8_t>(nextchar);
        }
    }

    return ulen;
}

bool huffman::is_compressed(const void* data) noexcept
{
    auto type = read_be16(data);
    return type == 0x30fb || type == 0x31fb || type == 0x32fb || type == 0x33fb
        || type == 0x34fb || type == 0x35fb
        || type == 0xb0fb || type == 0xb1fb || type == 0xb2fb || type == 0xb3fb
        || type == 0xb4fb || type == 0xb5fb;
}

int32_t huffman::decoded_size(const void* data, int32_t data_len) noexcept
{
    // With the 0x100 flag the field sits at offset 5 or 6, past the 8 bytes
    // callers guarantee, so bound the read instead of trusting the header.
    if (!data || data_len < 2) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    auto type = read_be16(data);
    auto ssize = (type & 0x8000) ? 4 : 3;
    const int32_t offset = (type & 0x100) ? 2 + ssize : 2;
    if (data_len - offset < 3) return 0;
    return static_cast<int32_t>(read_be24(p + offset));
}

int32_t huffman::decode(void* dest, int32_t dest_len, const void* source,
                        int32_t source_len, int32_t* source_size) noexcept
{
    auto ret = huff_decompress(static_cast<const uint8_t*>(source), source_len,
                               static_cast<uint8_t*>(dest), dest_len);
    if (source_size)
        *source_size = 0;
    return ret;
}

// ── Encode ─────────────────────────────────────────────────────────────────

// Huffman encode not needed for game data compatibility — we can decode
// existing compressed data. For encode, we delegate to a simple implementation.
// The original huffencode.cpp is large (~700 lines) and encode is rarely needed.
// We stub encode returning 0 (failure) for now.
int32_t huffman::encode(void* dest, const void* source, int32_t source_size) noexcept
{
    (void)dest;
    (void)source;
    (void)source_size;
    return 0; // encode not implemented — use refpack or zlib for compression
}

} // namespace compression::eac

#pragma warning(pop)

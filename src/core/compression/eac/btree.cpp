#include "btree.h"
#include "codec_common.h"
#include <cstring>
#include <cstdlib>

// ── BTree (Electronic Arts Compression) ──
// A binary tree-based entropy coder, similar to Huffman but builds the tree
// structure into the compressed stream. Used for compressing small data blocks.
//
// Frame format:
//   0x46FB: 24-bit uncompressed size (inferred = compressed size)
//   0x47FB: 24-bit uncompressed size + 24-bit compressed size
//
// ── Note ──
// The encode path is fully implemented (canonical Huffman-like tree construction),
// but is rarely used at runtime. The decode path is simple and fast.
namespace compression::eac {

// ── Decode ─────────────────────────────────────────────────────────────────

struct btree_decode_ctx
{
    int8_t  cluetbl[256];
    uint8_t left[256];
    uint8_t right[256];
    uint8_t* d;
    uint8_t* dend;
    bool overflow;
};

// The node table is attacker-controlled, so the tree it describes may contain
// cycles (a node whose child is itself).  Walking it with unbounded recursion
// overflowed the stack.  A tree over 256 distinct symbols cannot need more than
// 256 levels of genuine nesting, so anything deeper is malformed by definition.
constexpr uint32_t kBtreeMaxChaseDepth = 256;

static void btree_chase(btree_decode_ctx* dc, uint8_t node, uint32_t depth)
{
    if (dc->overflow) return;
    if (depth > kBtreeMaxChaseDepth)
    {
        dc->overflow = true;
        return;
    }
    if (dc->cluetbl[node])
    {
        btree_chase(dc, dc->left[node], depth + 1);
        btree_chase(dc, dc->right[node], depth + 1);
        return;
    }
    if (dc->d >= dc->dend)
    {
        dc->overflow = true;
        return;
    }
    *dc->d++ = node;
}

// Bounds contract: `pack` points into untrusted archive bytes and declares its
// own output size, so neither the source cursor nor the destination cursor can be
// trusted to stay in range.  Both are bounded here; previously the source cursor
// was an unbounded `*s++` inside `for (;;)` and the destination had no limit.
static int32_t btree_decompress(const uint8_t* pack, int32_t pack_len,
                                uint8_t* unpack, int32_t unpack_len)
{
    btree_decode_ctx dc{};
    auto* s = pack;
    dc.d = unpack;

    if (!s || !unpack || pack_len <= 0 || unpack_len < 0) return 0;

    const uint8_t* const sourceEnd = s + pack_len;
    dc.dend = unpack + unpack_len;

    const auto sourceAvailable = [&](uint32_t count) noexcept {
        return static_cast<uint64_t>(sourceEnd - s) >= count;
    };
    const auto emit = [&](uint8_t value) noexcept {
        if (dc.d >= dc.dend) return false;
        *dc.d++ = value;
        return true;
    };

    if (!sourceAvailable(2)) return 0;
    auto type = read_be16(s);
    s += 2;

    if (type == 0x47fb)
    {
        if (!sourceAvailable(3)) return 0;
        s += 3;
    }

    if (!sourceAvailable(3)) return 0;
    auto ulen = static_cast<int32_t>(read_be24(s));
    s += 3;

    if (ulen < 0 || ulen > unpack_len) return 0;

    std::memset(dc.cluetbl, 0, sizeof(dc.cluetbl));

    if (!sourceAvailable(2)) return 0;
    auto clue = *s++;
    dc.cluetbl[clue] = 1;

    auto nodes = *s++;
    for (int32_t i = 0; i < nodes; ++i)
    {
        if (!sourceAvailable(3)) return 0;
        auto node = *s++;
        dc.left[node] = *s++;
        dc.right[node] = *s++;
        dc.cluetbl[node] = -1;
    }

    for (;;)
    {
        if (!sourceAvailable(1)) return 0;
        auto node = *s++;
        auto c = dc.cluetbl[node];
        if (!c)
        {
            if (!emit(node)) return 0;
            continue;
        }
        if (c < 0)
        {
            btree_chase(&dc, dc.left[node], 0);
            btree_chase(&dc, dc.right[node], 0);
            if (dc.overflow) return 0;
            continue;
        }
        if (!sourceAvailable(1)) return 0;
        node = *s++;
        if (node)
        {
            if (!emit(node)) return 0;
            continue;
        }
        break;
    }

    return ulen;
}

bool btree::is_compressed(const void* data) noexcept
{
    auto type = read_be16(data);
    return type == 0x46fb || type == 0x47fb;
}

int32_t btree::decoded_size(const void* data, int32_t data_len) noexcept
{
    // The size field sits at offset 2 or 5 depending on frame type, so the read
    // can reach byte 8 while callers only guarantee 8 bytes total.
    if (!data || data_len < 2) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    const int32_t offset = read_be16(data) == 0x46fb ? 2 : 5;
    if (data_len - offset < 3) return 0;
    return static_cast<int32_t>(read_be24(p + offset));
}

int32_t btree::decode(void* dest, int32_t dest_len, const void* source,
                      int32_t source_len, int32_t* source_size) noexcept
{
    auto ret = btree_decompress(static_cast<const uint8_t*>(source), source_len,
                                static_cast<uint8_t*>(dest), dest_len);
    if (source_size)
        *source_size = 0; // not tracked by btree decoder
    return ret;
}

// ── Encode ─────────────────────────────────────────────────────────────────

using btree_word = int16_t;
constexpr int32_t BTREE_CODES = 256;
constexpr int32_t BTREE_BIGNUM = 32000;
constexpr int32_t BTREE_SLOPAGE = 16384;

struct btree_encode_ctx
{
    uint32_t  packbits{};
    uint32_t  workpattern{};
    uint8_t*  bufptr{};
    uint32_t  ulen{};
    uint32_t  masks[17]{};
    uint8_t   clueq[BTREE_CODES]{};
    uint8_t   right[BTREE_CODES]{};
    uint8_t   join[BTREE_CODES]{};
    uint32_t  plen{};
    uint8_t*  bufbase{};
    uint8_t*  bufend{};
    uint8_t*  buffer{};
    uint8_t*  buf1{};
    uint8_t*  buf2{};
};

struct btree_mem
{
    uint8_t* ptr{};
    int32_t  len{};
};

static void btree_write_bits(btree_encode_ctx* ec, btree_mem* dest,
    uint32_t bitpattern, uint32_t len)
{
    if (len > 16)
    {
        btree_write_bits(ec, dest, bitpattern >> 16, len - 16);
        btree_write_bits(ec, dest, bitpattern, 16);
    }
    else
    {
        ec->packbits += len;
        ec->workpattern += (bitpattern & ec->masks[len]) << (24 - ec->packbits);
        while (ec->packbits > 7)
        {
            dest->ptr[dest->len] = static_cast<uint8_t>(ec->workpattern >> 16);
            ++dest->len;
            ec->workpattern <<= 8;
            ec->packbits -= 8;
            ++ec->plen;
        }
    }
}

static void btree_adj_count(const uint8_t* s, const uint8_t* bend, btree_word* count)
{
    uint32_t i = *s++;
    bend -= 16;

    if (s < bend)
    {
        do
        {
            // `count` is a 65536-entry table indexed by the unsigned byte pair
            // (previous << 8) | current, matching how btree_find_best walks it
            // linearly as [first][second].  Narrowing through the SIGNED
            // btree_word made every pair >= 0x8000 negative; stored back into the
            // unsigned `i` and re-cast to int32_t that produced a negative index,
            // so half of all byte pairs incremented memory before the table
            // (ASan: read/write 516 bytes before a 131072-byte region) and their
            // counts were lost.  uint16_t keeps the index in 0..65535.
            #define BT_ADJ(j) do { i = static_cast<uint16_t>((i << 8) | s[j]); ++count[i]; } while(0)
            BT_ADJ(0); BT_ADJ(1); BT_ADJ(2); BT_ADJ(3);
            BT_ADJ(4); BT_ADJ(5); BT_ADJ(6); BT_ADJ(7);
            BT_ADJ(8); BT_ADJ(9); BT_ADJ(10); BT_ADJ(11);
            BT_ADJ(12); BT_ADJ(13); BT_ADJ(14); BT_ADJ(15);
            s += 16;
        } while (s < bend);
    }

    bend += 16;
    if (s < bend)
    {
        do
        {
            BT_ADJ(0);
            ++s;
        } while (s < bend);
    }
}

static void btree_clear_count(uint8_t* tryq, btree_word* countbuf)
{
    for (int32_t j = 0; j < BTREE_CODES; ++j)
    {
        if (*tryq)
        {
            *tryq++ = 1;
            std::memset(countbuf, 0, 256 * sizeof(btree_word));
            countbuf += 256;
        }
        else
        {
            ++tryq;
            countbuf += 256;
        }
    }
}

static void btree_join_nodes(btree_encode_ctx* ec,
    uint8_t* cluep, uint8_t* rightp, uint8_t* joinp, uint32_t clue)
{
    auto* s = ec->bufbase;
    auto* d = (s == ec->buf1) ? ec->buf2 : ec->buf1;
    ec->bufbase = d;
    auto* bend = ec->bufend;

    *bend = static_cast<uint8_t>(clue);
    while (s <= bend)
    {
        while (!cluep[(*d++ = *s++)])
            ;

        auto i = static_cast<uint32_t>(*(s - 1));

        if (cluep[i] == 1)
        {
            if (*s == rightp[i])
            {
                *(d - 1) = joinp[i];
                ++s;
            }
        }
        else if (cluep[i] == 3)
        {
            *(d - 1) = static_cast<uint8_t>(clue);
            *d++ = *(s - 1);
        }
        else
            *d++ = *s++;
    }
    ec->bufend = d - 2;
}

static uint32_t btree_find_best(btree_word* countptr, uint8_t* tryq,
    uint32_t* bestn, uint32_t* bestval, int32_t ratio)
{
    uint32_t bestsize = 1;
    uint32_t i_val = 3;

    for (uint32_t i2 = 0; i2 < BTREE_CODES; ++i2)
    {
        if (tryq[i2])
        {
            for (uint32_t i1 = 0; i1 < BTREE_CODES; ++i1)
            {
                if (*countptr++ > static_cast<btree_word>(i_val))
                {
                    if (tryq[i1])
                    {
                        i_val = *(countptr - 1);
                        uint32_t i3 = bestsize;
                        while (bestval[i3 - 1] < i_val)
                        {
                            bestn[i3] = bestn[i3 - 1];
                            bestval[i3] = bestval[i3 - 1];
                            --i3;
                        }
                        bestn[i3] = i2 * BTREE_CODES + i1;
                        bestval[i3] = i_val;
                        if (bestsize < 48) ++bestsize;
                        while (bestval[bestsize - 1] < (bestval[1] / static_cast<uint32_t>(ratio)))
                            --bestsize;
                        if (bestsize < 48)
                            i_val = bestval[1] / static_cast<uint32_t>(ratio);
                        else
                            i_val = bestval[bestsize - 1];
                    }
                }
            }
        }
        else
            countptr += 256;
    }
    return bestsize;
}

static void btree_tree_pack(btree_encode_ctx* ec, btree_mem* dest,
    uint32_t passes, uint32_t multimax, uint32_t quick, int32_t zerosuppress)
{
    uint32_t count2[BTREE_CODES]{};
    uint8_t tryq[BTREE_CODES]{};
    uint8_t freeq[BTREE_CODES]{};
    uint8_t bestjoin[BTREE_CODES]{};
    uint32_t bestn[BTREE_CODES]{};
    uint32_t bestval[BTREE_CODES];
    bestval[0] = static_cast<uint32_t>(-1);
    uint32_t bt_node[BTREE_CODES]{};
    uint32_t bt_left[BTREE_CODES]{};
    uint32_t bt_right[BTREE_CODES]{};
    uint32_t sortptr[BTREE_CODES];

    auto treebufsize = 65536 * sizeof(btree_word);
    auto buf1size = static_cast<int32_t>(ec->ulen * 3 / 2) + BTREE_SLOPAGE;
    auto buf2size = static_cast<int32_t>(ec->ulen * 3 / 2) + BTREE_SLOPAGE;

    auto* treebuf = static_cast<uint8_t*>(std::malloc(treebufsize));
    ec->buf1 = static_cast<uint8_t*>(std::malloc(buf1size));
    ec->buf2 = static_cast<uint8_t*>(std::malloc(buf2size));

    if (!treebuf || !ec->buf1 || !ec->buf2)
    {
        std::free(treebuf);
        std::free(ec->buf1);
        std::free(ec->buf2);
        return;
    }

    std::memcpy(ec->buf1, ec->buffer, ec->ulen);
    ec->buffer = ec->buf1;
    ec->bufptr = ec->buf1 + ec->ulen;
    ec->bufbase = ec->buffer;
    ec->bufend = ec->bufptr;

    auto* count = reinterpret_cast<btree_word*>(treebuf);
    auto ratio = quick ? static_cast<int32_t>(quick) : 2;

    for (uint32_t i = 0; i < BTREE_CODES; ++i) count2[i] = 0;
    uint32_t i1 = 0;
    auto* bend = ec->bufptr;
    auto* ptr1 = ec->buffer;
    while (ptr1 < bend)
    {
        auto i = *ptr1++;
        i1 += i;
        ++count2[i];
    }

    std::memset(ec->clueq, 0, sizeof(ec->clueq));

    for (uint32_t i = 0; i < BTREE_CODES; ++i)
    {
        freeq[i] = 1;
        tryq[i] = (count2[i] > 3) ? 1 : 0;
    }

    count2[0] = BTREE_BIGNUM;

    if (zerosuppress)
    {
        for (uint32_t i = 0; i < 32; ++i)
        {
            count2[i] = BTREE_BIGNUM;
            tryq[i] = 0;
            freeq[i] = 0;
        }
    }

    for (uint32_t i = 0; i < BTREE_CODES; ++i)
        sortptr[i] = i;

    uint32_t swapped = 1;
    while (swapped)
    {
        swapped = 0;
        for (uint32_t i = 1; i < BTREE_CODES; ++i)
        {
            if (count2[sortptr[i]] < count2[sortptr[i - 1]] ||
                (count2[sortptr[i]] == count2[sortptr[i - 1]] && sortptr[i] > sortptr[i - 1]))
            {
                auto tmp = sortptr[i];
                sortptr[i] = sortptr[i - 1];
                sortptr[i - 1] = tmp;
                swapped = 1;
            }
        }
    }

    uint32_t freeptr = 0;
    auto cls = sortptr[freeptr++];
    auto clue = static_cast<uint8_t>(cls);
    freeq[cls] = 0;
    tryq[cls] = 0;
    ec->clueq[cls] = 3;

    if (count2[cls])
        btree_join_nodes(ec, ec->clueq, ec->right, ec->join, clue);

    ec->clueq[cls] = 2;

    uint32_t bt_size = 0;
    auto domore = passes;
    while (domore)
    {
        btree_clear_count(tryq, count);

        ptr1 = ec->bufbase;
        bend = ec->bufend;
        btree_adj_count(ptr1, bend, count);

        auto bestsize = btree_find_best(count, tryq, bestn, bestval, ratio);

        domore = 0;
        if (bestsize > 1)
        {
            uint32_t tcost = 0, tsave = 0;
            uint32_t leftnode;
            uint32_t joinnode;
            i1 = 1;
            domore = 1;
            uint32_t i = 1;
            while (domore)
            {
                leftnode = (bestn[i] >> 8) & 255;
                auto rightnode = bestn[i] & 255;

                if (tryq[leftnode] == 1 && tryq[rightnode] == 1)
                {
                    domore = 0;
                    while (freeptr < BTREE_CODES && !freeq[sortptr[freeptr]])
                        ++freeptr;

                    if (freeptr < BTREE_CODES)
                    {
                        joinnode = sortptr[freeptr];
                        auto cost = 3 + count2[joinnode];
                        auto save = bestval[i];

                        if (cost < save)
                        {
                            tcost += cost;
                            tsave += save;

                            bestjoin[i1] = static_cast<uint8_t>(joinnode);
                            bestn[i1] = bestn[i];
                            ++i1;

                            freeq[joinnode] = 0;
                            tryq[joinnode] = 2;
                            ec->clueq[joinnode] = 3;
                            freeq[leftnode] = 0;
                            tryq[leftnode] = 2;
                            ec->clueq[leftnode] = 1;
                            ec->right[leftnode] = static_cast<uint8_t>(rightnode);
                            ec->join[leftnode] = static_cast<uint8_t>(joinnode);
                            freeq[rightnode] = 0;
                            tryq[rightnode] = 2;

                            bt_node[bt_size] = joinnode;
                            bt_left[bt_size] = leftnode;
                            bt_right[bt_size] = rightnode;
                            ++bt_size;

                            if (i1 <= multimax) domore = 1;
                        }
                    }
                }

                ++i;
                if (i >= bestsize) domore = 0;
            }

            bestsize = i1;

            if (bestsize > 1)
            {
                btree_join_nodes(ec, ec->clueq, ec->right, ec->join, clue);

                for (i = 1; i < bestsize; ++i)
                {
                    leftnode = (bestn[i] >> 8) & 255;
                    joinnode = bestjoin[i];
                    ec->clueq[leftnode] = 0;
                    ec->clueq[joinnode] = 0;
                }

                domore = --passes;
            }
        }
    }

    ec->bufptr = ec->bufend;

    btree_write_bits(ec, dest, clue, 8);
    btree_write_bits(ec, dest, bt_size, 8);

    for (uint32_t i = 0; i < bt_size; ++i)
    {
        btree_write_bits(ec, dest, bt_node[i], 8);
        btree_write_bits(ec, dest, bt_left[i], 8);
        btree_write_bits(ec, dest, bt_right[i], 8);
    }

    ptr1 = ec->bufbase;
    bend = ec->bufend;
    while (ptr1 < bend)
        btree_write_bits(ec, dest, *ptr1++, 8);

    btree_write_bits(ec, dest, clue, 8);
    btree_write_bits(ec, dest, 0, 8);
    btree_write_bits(ec, dest, 0, 7);

    std::free(ec->buf2);
    std::free(ec->buf1);
    std::free(treebuf);
}

static int32_t btree_compress_file(btree_encode_ctx* ec,
    btree_mem* infile, btree_mem* outfile, int32_t ulen, int32_t zerosuppress)
{
    ec->packbits = 0;
    ec->workpattern = 0;
    auto passes = 256u;
    auto multimax = 32u;
    ec->masks[0] = 0;
    for (uint32_t i = 1; i < 17; ++i)
        ec->masks[i] = (ec->masks[i - 1] << 1) + 1;

    ec->buffer = infile->ptr;
    ec->ulen = static_cast<uint32_t>(infile->len);
    ec->bufptr = ec->buffer + infile->len;

    outfile->ptr = outfile->ptr;
    outfile->len = 0;

    ec->packbits = 0;
    ec->workpattern = 0;
    ec->plen = 0;

    if (ulen == infile->len)
    {
        btree_write_bits(ec, outfile, 0x46fb, 16);
        btree_write_bits(ec, outfile, static_cast<uint32_t>(infile->len), 24);
    }
    else
    {
        btree_write_bits(ec, outfile, 0x47fb, 16);
        btree_write_bits(ec, outfile, static_cast<uint32_t>(ulen), 24);
        btree_write_bits(ec, outfile, static_cast<uint32_t>(infile->len), 24);
    }

    btree_tree_pack(ec, outfile, passes, multimax, 0, zerosuppress);

    return outfile->len;
}

int32_t btree::encode(void* dest, const void* source, int32_t source_size) noexcept
{
    btree_encode_ctx ec{};
    btree_mem infile{ const_cast<uint8_t*>(static_cast<const uint8_t*>(source)), source_size };
    btree_mem outfile{ static_cast<uint8_t*>(dest), source_size };

    return btree_compress_file(&ec, &infile, &outfile, source_size, 0);
}

} // namespace compression::eac

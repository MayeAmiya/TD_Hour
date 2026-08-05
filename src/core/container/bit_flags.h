#pragma once

#include "container/container_types.h"

#include <bitset>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <algorithm>

#include "debug/td_assert.h"

namespace container {

template<size_t N, const container::Array<const char*, N>* NAMES = nullptr>
class bit_flags
{
public:
    using bitset = std::bitset<N>;

    bit_flags() = default;

    bit_flags(std::initializer_list<size_t> bits)
    {
        for (auto b : bits)
        {
            TD_ASSERT(b < N);
            m_bits.set(b);
        }
    }

    explicit bit_flags(size_t bit)
    {
        TD_ASSERT(bit < N);
        m_bits.set(bit);
    }

    explicit bit_flags(const bitset& bits) : m_bits(bits) {}

    bit_flags& set(size_t pos, bool value = true)
    {
        TD_ASSERT(pos < N);
        m_bits.set(pos, value);
        return *this;
    }

    bit_flags& reset(size_t pos)
    {
        TD_ASSERT(pos < N);
        m_bits.reset(pos);
        return *this;
    }

    bit_flags& flip(size_t pos)
    {
        TD_ASSERT(pos < N);
        m_bits.flip(pos);
        return *this;
    }

    bit_flags& flip()
    {
        m_bits.flip();
        return *this;
    }

    [[nodiscard]] bool test(size_t pos) const
    {
        TD_ASSERT(pos < N);
        return m_bits.test(pos);
    }

    void clear() { m_bits.reset(); }

    void clear(int bit)
    {
        TD_ASSERT(bit >= 0 && bit < static_cast<int>(N));
        m_bits.reset(static_cast<size_t>(bit));
    }

    void set_all() { m_bits.set(); }

    static constexpr size_t size() noexcept { return N; }
    [[nodiscard]] size_t count() const { return m_bits.count(); }
    [[nodiscard]] bool any() const { return m_bits.any(); }
    [[nodiscard]] bool none() const { return m_bits.none(); }
    [[nodiscard]] bool all() const { return m_bits.all(); }

    [[nodiscard]] bit_flags operator&(const bit_flags& rhs) const { return bit_flags(m_bits & rhs.m_bits); }
    [[nodiscard]] bit_flags operator|(const bit_flags& rhs) const { return bit_flags(m_bits | rhs.m_bits); }
    [[nodiscard]] bit_flags operator^(const bit_flags& rhs) const { return bit_flags(m_bits ^ rhs.m_bits); }
    [[nodiscard]] bit_flags operator~() const { return bit_flags(~m_bits); }

    bit_flags& operator&=(const bit_flags& rhs) { m_bits &= rhs.m_bits; return *this; }
    bit_flags& operator|=(const bit_flags& rhs) { m_bits |= rhs.m_bits; return *this; }
    bit_flags& operator^=(const bit_flags& rhs) { m_bits ^= rhs.m_bits; return *this; }

    bool operator==(const bit_flags& rhs) const { return m_bits == rhs.m_bits; }
    bool operator!=(const bit_flags& rhs) const { return m_bits != rhs.m_bits; }

    [[nodiscard]] bool test_for_any(const bit_flags& that) const
    {
        return (m_bits & that.m_bits).any();
    }

    [[nodiscard]] bool test_for_all(const bit_flags& that) const
    {
        return (m_bits & that.m_bits) == that.m_bits;
    }

    [[nodiscard]] bool test_for_none(const bit_flags& that) const
    {
        return (m_bits & that.m_bits).none();
    }

    [[nodiscard]] size_t count_intersection(const bit_flags& that) const
    {
        return (m_bits & that.m_bits).count();
    }

    [[nodiscard]] size_t count_difference(const bit_flags& that) const
    {
        return (~m_bits & that.m_bits).count();
    }

    // read-only access to underlying bitset
    const bitset& raw() const noexcept { return m_bits; }

private:
    bitset m_bits;
};

template<size_t N, const container::Array<const char*, N>* Names = nullptr>
using BitFlags = bit_flags<N, Names>;

} // namespace container

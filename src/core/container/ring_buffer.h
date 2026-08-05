#pragma once

#include "container/container_types.h"

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <iterator>
#include <optional>
#include <utility>

namespace container {

template<typename T, size_t CAPACITY>
class ring_buffer
{
    static_assert(CAPACITY > 0, "CAPACITY must be > 0");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");

    // optional owns the lifetime of each fixed slot while allowing emplace()
    // to construct T directly in its final address. The previous T array
    // default-constructed every slot and then assigned from a temporary.
    container::Array<std::optional<T>, CAPACITY> m_buffer{};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;

public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;

    ring_buffer() = default;

    explicit ring_buffer(const T& default_value)
    {
        for (size_t i = 0; i < CAPACITY; ++i)
        {
            m_buffer[i].emplace(default_value);
        }
    }

    static constexpr size_type capacity() noexcept { return CAPACITY; }
    [[nodiscard]] size_type size() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] bool full() const noexcept { return m_count >= CAPACITY; }

    bool push(const T& item) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        if (m_count >= CAPACITY) return false;
        m_buffer[m_head].emplace(item);
        m_head = (m_head + 1) % CAPACITY;
        ++m_count;
        return true;
    }

    bool push(T&& item) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (m_count >= CAPACITY) return false;
        m_buffer[m_head].emplace(std::move(item));
        m_head = (m_head + 1) % CAPACITY;
        ++m_count;
        return true;
    }

    template<typename... Args>
    bool emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        if (m_count >= CAPACITY) return false;
        m_buffer[m_head].emplace(std::forward<Args>(args)...);
        m_head = (m_head + 1) % CAPACITY;
        ++m_count;
        return true;
    }

    bool pop(T& item) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        if (m_count == 0) return false;
        item = std::move(*m_buffer[m_tail]);
        m_buffer[m_tail].reset();
        m_tail = (m_tail + 1) % CAPACITY;
        --m_count;
        return true;
    }

    bool pop_front() noexcept
    {
        if (m_count == 0) return false;
        m_buffer[m_tail].reset();
        m_tail = (m_tail + 1) % CAPACITY;
        --m_count;
        return true;
    }

    void clear() noexcept
    {
        while (m_count != 0)
        {
            m_buffer[m_tail].reset();
            m_tail = (m_tail + 1) % CAPACITY;
            --m_count;
        }
        m_head = m_tail = 0;
    }

    reference front() noexcept { return *m_buffer[m_tail]; }
    const_reference front() const noexcept { return *m_buffer[m_tail]; }

    reference back() noexcept { return *m_buffer[(m_head + CAPACITY - 1) % CAPACITY]; }
    const_reference back() const noexcept { return *m_buffer[(m_head + CAPACITY - 1) % CAPACITY]; }

    reference operator[](size_type idx) noexcept { return *m_buffer[(m_tail + idx) % CAPACITY]; }
    const_reference operator[](size_type idx) const noexcept { return *m_buffer[(m_tail + idx) % CAPACITY]; }

    class iterator
    {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() noexcept : m_buf(nullptr), m_index(0) {}
        iterator(ring_buffer* buf, size_type idx) noexcept : m_buf(buf), m_index(idx) {}

        reference operator*() const noexcept { return (*m_buf)[m_index]; }
        pointer operator->() const noexcept { return &(*m_buf)[m_index]; }

        iterator& operator++() noexcept { ++m_index; return *this; }
        iterator operator++(int) noexcept { auto tmp = *this; ++m_index; return tmp; }
        iterator& operator--() noexcept { --m_index; return *this; }
        iterator operator--(int) noexcept { auto tmp = *this; --m_index; return tmp; }

        iterator& operator+=(difference_type n) noexcept { m_index += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { m_index -= n; return *this; }
        iterator operator+(difference_type n) const noexcept { return iterator(m_buf, m_index + n); }
        iterator operator-(difference_type n) const noexcept { return iterator(m_buf, m_index - n); }
        difference_type operator-(const iterator& o) const noexcept { return static_cast<difference_type>(m_index - o.m_index); }

        reference operator[](difference_type n) const noexcept { return (*m_buf)[m_index + n]; }

        bool operator==(const iterator& o) const noexcept { return m_index == o.m_index; }
        bool operator!=(const iterator& o) const noexcept { return m_index != o.m_index; }
        bool operator<(const iterator& o) const noexcept { return m_index < o.m_index; }
        bool operator<=(const iterator& o) const noexcept { return m_index <= o.m_index; }
        bool operator>(const iterator& o) const noexcept { return m_index > o.m_index; }
        bool operator>=(const iterator& o) const noexcept { return m_index >= o.m_index; }

    private:
        ring_buffer* m_buf;
        size_type m_index;
        friend class const_iterator;
    };

    class const_iterator
    {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() noexcept : m_buf(nullptr), m_index(0) {}
        const_iterator(const ring_buffer* buf, size_type idx) noexcept : m_buf(buf), m_index(idx) {}
        const_iterator(const iterator& it) noexcept : m_buf(it.m_buf), m_index(it.m_index) {}

        reference operator*() const noexcept { return (*m_buf)[m_index]; }
        pointer operator->() const noexcept { return &(*m_buf)[m_index]; }

        const_iterator& operator++() noexcept { ++m_index; return *this; }
        const_iterator operator++(int) noexcept { auto tmp = *this; ++m_index; return tmp; }
        const_iterator& operator--() noexcept { --m_index; return *this; }
        const_iterator operator--(int) noexcept { auto tmp = *this; --m_index; return tmp; }

        const_iterator& operator+=(difference_type n) noexcept { m_index += n; return *this; }
        const_iterator& operator-=(difference_type n) noexcept { m_index -= n; return *this; }
        const_iterator operator+(difference_type n) const noexcept { return const_iterator(m_buf, m_index + n); }
        const_iterator operator-(difference_type n) const noexcept { return const_iterator(m_buf, m_index - n); }
        difference_type operator-(const const_iterator& o) const noexcept { return static_cast<difference_type>(m_index - o.m_index); }

        reference operator[](difference_type n) const noexcept { return (*m_buf)[m_index + n]; }

        bool operator==(const const_iterator& o) const noexcept { return m_index == o.m_index; }
        bool operator!=(const const_iterator& o) const noexcept { return m_index != o.m_index; }
        bool operator<(const const_iterator& o) const noexcept { return m_index < o.m_index; }
        bool operator<=(const const_iterator& o) const noexcept { return m_index <= o.m_index; }
        bool operator>(const const_iterator& o) const noexcept { return m_index > o.m_index; }
        bool operator>=(const const_iterator& o) const noexcept { return m_index >= o.m_index; }

    private:
        const ring_buffer* m_buf;
        size_type m_index;
    };

    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, m_count); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, m_count); }
    const_iterator cbegin() const noexcept { return const_iterator(this, 0); }
    const_iterator cend() const noexcept { return const_iterator(this, m_count); }
};

template<typename T, size_t Capacity>
using RingBuffer = ring_buffer<T, Capacity>;

} // namespace container

#pragma once

#include "container/container_types.h"

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <type_traits>
#include <utility>

#include "debug/td_assert.h"

namespace container {

template<typename T>
class sparse_set
{
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

    container::Vector<T> m_dense;
    container::Vector<uint32_t> m_dense_id;
    container::Vector<uint32_t> m_sparse;
    uint32_t m_max_id = 0;

    [[noreturn]] static void fail_invalid_id(const char* operation) noexcept
    {
        ::debug::detail::assertion_failed(
            "id < m_max_id", operation, __FILE__, __LINE__);
    }

public:
    using value_type = T;
    using size_type = size_t;
    using iterator = typename container::Vector<T>::iterator;
    using const_iterator = typename container::Vector<T>::const_iterator;

    explicit sparse_set(uint32_t max_entities)
        : m_max_id(max_entities)
    {
        m_sparse.resize(max_entities, UINT32_MAX);
    }

    [[nodiscard]] size_type size() const noexcept { return m_dense.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_dense.empty(); }
    [[nodiscard]] uint32_t max_id() const noexcept { return m_max_id; }

    T& get(uint32_t id)
    {
        if (!has(id)) fail_invalid_id("Entity not in SparseSet");
        return m_dense[m_sparse[id]];
    }

    const T& get(uint32_t id) const
    {
        if (!has(id)) fail_invalid_id("Entity not in SparseSet");
        return m_dense[m_sparse[id]];
    }

    T* try_get(uint32_t id) noexcept
    {
        if (!has(id)) return nullptr;
        return &m_dense[m_sparse[id]];
    }

    const T* try_get(uint32_t id) const noexcept
    {
        if (!has(id)) return nullptr;
        return &m_dense[m_sparse[id]];
    }

    T& add(uint32_t id, const T& value = T{})
    {
        T* result = try_add(id, value);
        if (!result) fail_invalid_id("SparseSet add ID exceeds capacity");
        return *result;
    }

    T* try_add(uint32_t id, const T& value = T{})
    {
        if (id >= m_max_id) return nullptr;
        if (has(id))
        {
            m_dense[m_sparse[id]] = value;
            return &m_dense[m_sparse[id]];
        }

        uint32_t idx = static_cast<uint32_t>(m_dense.size());
        m_sparse[id] = idx;
        m_dense.push_back(value);
        m_dense_id.push_back(id);
        return &m_dense.back();
    }

    T& add(uint32_t id, T&& value)
    {
        T* result = try_add(id, std::move(value));
        if (!result) fail_invalid_id("SparseSet add ID exceeds capacity");
        return *result;
    }

    T* try_add(uint32_t id, T&& value)
    {
        if (id >= m_max_id) return nullptr;
        if (has(id))
        {
            m_dense[m_sparse[id]] = std::move(value);
            return &m_dense[m_sparse[id]];
        }

        uint32_t idx = static_cast<uint32_t>(m_dense.size());
        m_sparse[id] = idx;
        m_dense.push_back(std::move(value));
        m_dense_id.push_back(id);
        return &m_dense.back();
    }

    template<typename... Args>
    T& emplace(uint32_t id, Args&&... args)
    {
        T* result = try_emplace(id, std::forward<Args>(args)...);
        if (!result) fail_invalid_id("SparseSet emplace ID exceeds capacity");
        return *result;
    }

    template<typename... Args>
    T* try_emplace(uint32_t id, Args&&... args)
    {
        if (id >= m_max_id) return nullptr;
        if (has(id))
        {
            m_dense[m_sparse[id]] = T(std::forward<Args>(args)...);
            return &m_dense[m_sparse[id]];
        }

        uint32_t idx = static_cast<uint32_t>(m_dense.size());
        m_sparse[id] = idx;
        m_dense.emplace_back(std::forward<Args>(args)...);
        m_dense_id.push_back(id);
        return &m_dense.back();
    }

    void remove(uint32_t id)
    {
        if (!has(id)) return;

        uint32_t idx = m_sparse[id];
        uint32_t last = static_cast<uint32_t>(m_dense.size() - 1);

        if (idx != last)
        {
            m_dense[idx] = std::move(m_dense[last]);
            m_dense_id[idx] = m_dense_id[last];
            m_sparse[m_dense_id[idx]] = idx;
        }

        m_dense.pop_back();
        m_dense_id.pop_back();
        m_sparse[id] = UINT32_MAX;
    }

    void clear() noexcept
    {
        m_dense.clear();
        m_dense_id.clear();
        std::fill(m_sparse.begin(), m_sparse.end(), UINT32_MAX);
    }

    [[nodiscard]] bool has(uint32_t id) const noexcept
    {
        return id < m_max_id && m_sparse[id] < static_cast<uint32_t>(m_dense.size());
    }

    T* data() noexcept { return m_dense.data(); }
    const T* data() const noexcept { return m_dense.data(); }
    uint32_t* id_data() noexcept { return m_dense_id.data(); }
    const uint32_t* id_data() const noexcept { return m_dense_id.data(); }

    iterator begin() noexcept { return m_dense.begin(); }
    iterator end() noexcept { return m_dense.end(); }
    const_iterator begin() const noexcept { return m_dense.begin(); }
    const_iterator end() const noexcept { return m_dense.end(); }
    const_iterator cbegin() const noexcept { return m_dense.cbegin(); }
    const_iterator cend() const noexcept { return m_dense.cend(); }

    bool intersects(const sparse_set& other) const noexcept
    {
        const auto& smaller = m_dense.size() <= other.m_dense.size() ? *this : other;
        const auto& larger  = m_dense.size() <= other.m_dense.size() ? other : *this;

        for (uint32_t id : smaller.m_dense_id)
        {
            if (larger.has(id)) return true;
        }
        return false;
    }

    container::Vector<uint32_t> intersection_ids(const sparse_set& other) const
    {
        container::Vector<uint32_t> result;
        const auto& smaller = m_dense.size() <= other.m_dense.size() ? *this : other;
        const auto& larger  = m_dense.size() <= other.m_dense.size() ? other : *this;

        for (uint32_t id : smaller.m_dense_id)
        {
            if (larger.has(id)) result.push_back(id);
        }
        return result;
    }

    container::Vector<uint32_t> difference_ids(const sparse_set& other) const
    {
        container::Vector<uint32_t> result;
        for (uint32_t id : m_dense_id)
        {
            if (!other.has(id)) result.push_back(id);
        }
        return result;
    }
};

template<typename T>
using SparseSet = sparse_set<T>;

} // namespace container

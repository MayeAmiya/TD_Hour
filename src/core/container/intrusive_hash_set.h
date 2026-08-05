#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <functional>

namespace container {

template<typename T>
struct intrusive_hash_set_node
{
    intrusive_hash_set_node* m_hash_next = nullptr;
    uint32_t m_hash_key = 0;

    T* hash_next() noexcept { return static_cast<T*>(m_hash_next); }
    const T* hash_next() const noexcept { return static_cast<const T*>(m_hash_next); }
};

template<typename T, size_t BUCKETS = 1024>
class intrusive_hash_set
{
    static_assert(std::is_base_of_v<intrusive_hash_set_node<T>, T>,
                  "T must inherit from intrusive_hash_set_node<T>");

    using node = intrusive_hash_set_node<T>;

    node* m_buckets[BUCKETS] = {};
    size_t m_size = 0;

    static size_t hash(uint32_t key) noexcept { return key % BUCKETS; }

    static node* to_node(T* item) noexcept { return static_cast<node*>(item); }
    static const node* to_node(const T* item) noexcept { return static_cast<const node*>(item); }

public:
    intrusive_hash_set() = default;

    ~intrusive_hash_set()
    {
        for (size_t i = 0; i < BUCKETS; ++i)
        {
            node* cur = m_buckets[i];
            while (cur)
            {
                node* next = cur->m_hash_next;
                cur->m_hash_next = nullptr;
                cur->m_hash_key = 0;
                cur = next;
            }
        }
    }

    intrusive_hash_set(const intrusive_hash_set&) = delete;
    intrusive_hash_set& operator=(const intrusive_hash_set&) = delete;

    intrusive_hash_set(intrusive_hash_set&& other) noexcept : m_size(other.m_size)
    {
        for (size_t i = 0; i < BUCKETS; ++i)
        {
            m_buckets[i] = other.m_buckets[i];
            other.m_buckets[i] = nullptr;
        }
        other.m_size = 0;
    }

    intrusive_hash_set& operator=(intrusive_hash_set&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            for (size_t i = 0; i < BUCKETS; ++i)
            {
                m_buckets[i] = other.m_buckets[i];
                other.m_buckets[i] = nullptr;
            }
            m_size = other.m_size;
            other.m_size = 0;
        }
        return *this;
    }

    [[nodiscard]] size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    static constexpr size_t bucket_count() noexcept { return BUCKETS; }

    void insert(T* item, uint32_t key) noexcept
    {
        node* nd = to_node(item);

        node* cur = m_buckets[hash(key)];
        while (cur)
        {
            if (cur == nd) return;
            cur = cur->m_hash_next;
        }

        nd->m_hash_key = key;
        size_t idx = hash(key);
        nd->m_hash_next = m_buckets[idx];
        m_buckets[idx] = nd;
        ++m_size;
    }

    bool remove(T* item) noexcept
    {
        node* nd = to_node(item);
        size_t idx = hash(nd->m_hash_key);
        node** pp = &m_buckets[idx];

        while (*pp)
        {
            if (*pp == nd)
            {
                *pp = nd->m_hash_next;
                nd->m_hash_next = nullptr;
                --m_size;
                return true;
            }
            pp = &(*pp)->m_hash_next;
        }
        return false;
    }

    void clear() noexcept
    {
        for (size_t i = 0; i < BUCKETS; ++i)
        {
            node* cur = m_buckets[i];
            while (cur)
            {
                node* next = cur->m_hash_next;
                cur->m_hash_next = nullptr;
                cur = next;
            }
            m_buckets[i] = nullptr;
        }
        m_size = 0;
    }

    T* find(uint32_t key) noexcept
    {
        node* n = m_buckets[hash(key)];
        while (n)
        {
            if (n->m_hash_key == key) return static_cast<T*>(n);
            n = n->m_hash_next;
        }
        return nullptr;
    }

    const T* find(uint32_t key) const noexcept
    {
        const node* n = m_buckets[hash(key)];
        while (n)
        {
            if (n->m_hash_key == key) return static_cast<const T*>(n);
            n = n->m_hash_next;
        }
        return nullptr;
    }

    bool contains(const T* item) const noexcept
    {
        const node* nd = to_node(item);
        const node* cur = m_buckets[hash(nd->m_hash_key)];
        while (cur)
        {
            if (cur == nd) return true;
            cur = cur->m_hash_next;
        }
        return false;
    }

    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() noexcept : m_bucket(0), m_node(nullptr), m_set(nullptr) {}

        reference operator*() const noexcept { return *static_cast<T*>(m_node); }
        pointer operator->() const noexcept { return static_cast<T*>(m_node); }

        iterator& operator++() noexcept
        {
            if (m_node) m_node = m_node->m_hash_next;
            if (!m_node)
            {
                ++m_bucket;
                while (m_bucket < BUCKETS && !m_set->m_buckets[m_bucket])
                {
                    ++m_bucket;
                }
                if (m_bucket < BUCKETS)
                {
                    m_node = m_set->m_buckets[m_bucket];
                }
            }
            return *this;
        }

        iterator operator++(int) noexcept { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const iterator& o) const noexcept { return m_node == o.m_node; }
        bool operator!=(const iterator& o) const noexcept { return m_node != o.m_node; }

    private:
        friend class intrusive_hash_set;
        size_t m_bucket;
        node* m_node;
        intrusive_hash_set* m_set;

        iterator(intrusive_hash_set* set) noexcept : m_bucket(BUCKETS), m_node(nullptr), m_set(set)
        {
            for (size_t i = 0; i < BUCKETS; ++i)
            {
                if (set->m_buckets[i])
                {
                    m_bucket = i;
                    m_node = set->m_buckets[i];
                    break;
                }
            }
        }
    };

    class const_iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() noexcept : m_bucket(0), m_node(nullptr), m_set(nullptr) {}
        const_iterator(const iterator& it) noexcept : m_bucket(it.m_bucket), m_node(it.m_node), m_set(it.m_set) {}

        reference operator*() const noexcept { return *static_cast<const T*>(m_node); }
        pointer operator->() const noexcept { return static_cast<const T*>(m_node); }

        const_iterator& operator++() noexcept
        {
            if (m_node) m_node = m_node->m_hash_next;
            if (!m_node)
            {
                ++m_bucket;
                while (m_bucket < BUCKETS && !m_set->m_buckets[m_bucket])
                {
                    ++m_bucket;
                }
                if (m_bucket < BUCKETS)
                {
                    m_node = m_set->m_buckets[m_bucket];
                }
            }
            return *this;
        }

        const_iterator operator++(int) noexcept { auto tmp = *this; ++(*this); return tmp; }

        bool operator==(const const_iterator& o) const noexcept { return m_node == o.m_node; }
        bool operator!=(const const_iterator& o) const noexcept { return m_node != o.m_node; }

    private:
        friend class intrusive_hash_set;
        size_t m_bucket;
        const node* m_node;
        const intrusive_hash_set* m_set;

        const_iterator(const intrusive_hash_set* set) noexcept : m_bucket(BUCKETS), m_node(nullptr), m_set(set)
        {
            for (size_t i = 0; i < BUCKETS; ++i)
            {
                if (set->m_buckets[i])
                {
                    m_bucket = i;
                    m_node = set->m_buckets[i];
                    break;
                }
            }
        }
    };

    iterator begin() noexcept { return iterator(this); }
    iterator end() noexcept { return iterator(); }
    const_iterator begin() const noexcept { return const_iterator(this); }
    const_iterator end() const noexcept { return const_iterator(); }
    const_iterator cbegin() const noexcept { return const_iterator(this); }
    const_iterator cend() const noexcept { return const_iterator(); }
};

template<typename T>
using IntrusiveHashSetNode = intrusive_hash_set_node<T>;

template<typename T, size_t BucketCount = 1024>
using IntrusiveHashSet = intrusive_hash_set<T, BucketCount>;

} // namespace container

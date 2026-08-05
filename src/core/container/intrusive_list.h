#pragma once

#include <cstdint>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace container {

template<typename T>
struct intrusive_list_node
{
    intrusive_list_node* m_next = nullptr;
    intrusive_list_node* m_prev = nullptr;

    void remove_from_list() noexcept
    {
        if (m_prev) m_prev->m_next = m_next;
        if (m_next) m_next->m_prev = m_prev;
        m_next = nullptr;
        m_prev = nullptr;
    }

    void insert_after(intrusive_list_node* node) noexcept
    {
        m_next = node->m_next;
        m_prev = node;
        if (node->m_next) node->m_next->m_prev = this;
        node->m_next = this;
    }

    void insert_before(intrusive_list_node* node) noexcept
    {
        m_next = node;
        m_prev = node->m_prev;
        if (node->m_prev) node->m_prev->m_next = this;
        node->m_prev = this;
    }

    [[nodiscard]] bool is_linked() const noexcept { return m_next != nullptr; }

    [[nodiscard]] T* next() noexcept { return static_cast<T*>(m_next); }
    [[nodiscard]] const T* next() const noexcept { return static_cast<const T*>(m_next); }
    [[nodiscard]] T* prev() noexcept { return static_cast<T*>(m_prev); }
    [[nodiscard]] const T* prev() const noexcept { return static_cast<const T*>(m_prev); }
};

template<typename T>
class intrusive_list
{
    static_assert(std::is_base_of_v<intrusive_list_node<T>, T>,
                  "T must inherit from intrusive_list_node<T>");

    using node = intrusive_list_node<T>;

    node m_head;

    static node* to_node(T* item) noexcept { return static_cast<node*>(item); }
    static const node* to_node(const T* item) noexcept { return static_cast<const node*>(item); }

public:
    intrusive_list() noexcept
    {
        m_head.m_next = &m_head;
        m_head.m_prev = &m_head;
    }

    ~intrusive_list()
    {
        node* cur = m_head.m_next;
        while (cur != &m_head)
        {
            node* next = cur->m_next;
            cur->m_next = nullptr;
            cur->m_prev = nullptr;
            cur = next;
        }
    }

    intrusive_list(const intrusive_list&) = delete;
    intrusive_list& operator=(const intrusive_list&) = delete;

    intrusive_list(intrusive_list&& other) noexcept : m_head()
    {
        m_head.m_next = &m_head;
        m_head.m_prev = &m_head;

        if (!other.empty())
        {
            m_head.m_next = other.m_head.m_next;
            m_head.m_prev = other.m_head.m_prev;
            m_head.m_next->m_prev = &m_head;
            m_head.m_prev->m_next = &m_head;

            other.m_head.m_next = &other.m_head;
            other.m_head.m_prev = &other.m_head;
        }
    }

    intrusive_list& operator=(intrusive_list&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            if (!other.empty())
            {
                m_head.m_next = other.m_head.m_next;
                m_head.m_prev = other.m_head.m_prev;
                m_head.m_next->m_prev = &m_head;
                m_head.m_prev->m_next = &m_head;

                other.m_head.m_next = &other.m_head;
                other.m_head.m_prev = &other.m_head;
            }
        }
        return *this;
    }

    [[nodiscard]] T* front() noexcept
    {
        return m_head.m_next != &m_head ? static_cast<T*>(m_head.m_next) : nullptr;
    }

    [[nodiscard]] const T* front() const noexcept
    {
        return m_head.m_next != &m_head ? static_cast<const T*>(m_head.m_next) : nullptr;
    }

    [[nodiscard]] T* back() noexcept
    {
        return m_head.m_prev != &m_head ? static_cast<T*>(m_head.m_prev) : nullptr;
    }

    [[nodiscard]] const T* back() const noexcept
    {
        return m_head.m_prev != &m_head ? static_cast<const T*>(m_head.m_prev) : nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return m_head.m_next == &m_head; }

    [[nodiscard]] size_t size() const noexcept
    {
        size_t count = 0;
        const node* n = m_head.m_next;
        while (n != &m_head)
        {
            ++count;
            n = n->m_next;
        }
        return count;
    }

    void push_front(T* item) noexcept
    {
        to_node(item)->insert_after(&m_head);
    }

    void push_back(T* item) noexcept
    {
        to_node(item)->insert_before(&m_head);
    }

    void remove(T* item) noexcept
    {
        to_node(item)->remove_from_list();
    }

    T* pop_front() noexcept
    {
        if (empty()) return nullptr;
        T* item = front();
        remove(item);
        return item;
    }

    T* pop_back() noexcept
    {
        if (empty()) return nullptr;
        T* item = back();
        remove(item);
        return item;
    }

    void clear() noexcept
    {
        node* cur = m_head.m_next;
        while (cur != &m_head)
        {
            node* next = cur->m_next;
            cur->m_next = nullptr;
            cur->m_prev = nullptr;
            cur = next;
        }
        m_head.m_next = &m_head;
        m_head.m_prev = &m_head;
    }

    void splice(intrusive_list&& other) noexcept
    {
        if (other.empty()) return;

        node* first = other.m_head.m_next;
        node* last = other.m_head.m_prev;

        last->m_next = &m_head;
        first->m_prev = m_head.m_prev;
        m_head.m_prev->m_next = first;
        m_head.m_prev = last;

        other.m_head.m_next = &other.m_head;
        other.m_head.m_prev = &other.m_head;
    }

    void swap(intrusive_list& other) noexcept
    {
        intrusive_list tmp = std::move(other);
        other = std::move(*this);
        *this = std::move(tmp);
    }

    class iterator
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() noexcept : m_node(nullptr), m_head(nullptr) {}
        iterator(node* n, node* h) noexcept : m_node(n), m_head(h) {}

        reference operator*() const noexcept { return *static_cast<T*>(m_node); }
        pointer operator->() const noexcept { return static_cast<T*>(m_node); }

        iterator& operator++() noexcept { m_node = m_node->m_next; return *this; }
        iterator operator++(int) noexcept { auto tmp = *this; m_node = m_node->m_next; return tmp; }
        iterator& operator--() noexcept { m_node = m_node->m_prev; return *this; }
        iterator operator--(int) noexcept { auto tmp = *this; m_node = m_node->m_prev; return tmp; }

        bool operator==(const iterator& o) const noexcept { return m_node == o.m_node; }
        bool operator!=(const iterator& o) const noexcept { return m_node != o.m_node; }

    private:
        node* m_node;
        node* m_head;
    };

    class const_iterator
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = const T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() noexcept : m_node(nullptr), m_head(nullptr) {}
        const_iterator(const node* n, const node* h) noexcept : m_node(n), m_head(h) {}
        const_iterator(const iterator& it) noexcept : m_node(it.m_node), m_head(it.m_head) {}

        reference operator*() const noexcept { return *static_cast<const T*>(m_node); }
        pointer operator->() const noexcept { return static_cast<const T*>(m_node); }

        const_iterator& operator++() noexcept { m_node = m_node->m_next; return *this; }
        const_iterator operator++(int) noexcept { auto tmp = *this; m_node = m_node->m_next; return tmp; }
        const_iterator& operator--() noexcept { m_node = m_node->m_prev; return *this; }
        const_iterator operator--(int) noexcept { auto tmp = *this; m_node = m_node->m_prev; return tmp; }

        bool operator==(const const_iterator& o) const noexcept { return m_node == o.m_node; }
        bool operator!=(const const_iterator& o) const noexcept { return m_node != o.m_node; }

    private:
        const node* m_node;
        const node* m_head;
        friend class iterator;
    };

    iterator begin() noexcept { return iterator(m_head.m_next, &m_head); }
    iterator end() noexcept { return iterator(&m_head, &m_head); }
    const_iterator begin() const noexcept { return const_iterator(m_head.m_next, &m_head); }
    const_iterator end() const noexcept { return const_iterator(&m_head, &m_head); }
    const_iterator cbegin() const noexcept { return const_iterator(m_head.m_next, &m_head); }
    const_iterator cend() const noexcept { return const_iterator(&m_head, &m_head); }
};

template<typename T>
using IntrusiveListNode = intrusive_list_node<T>;

template<typename T>
using IntrusiveList = intrusive_list<T>;

} // namespace container

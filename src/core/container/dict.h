#pragma once

#include <variant>
#include "hash_containers.h"
#include <cstdint>
#include <algorithm>

#include "debug/td_assert.h"
#include "string_id.h"

namespace container {

class dict_value
{
public:
    enum type_ : uint8_t
    {
        none   = 0,
        boolean,
        integer,
        real_,
        string,
    };

    dict_value() : m_type(none), m_int(0) {}

    explicit dict_value(bool v)       : m_type(boolean), m_bool(v) {}
    explicit dict_value(int v)        : m_type(integer), m_int(v) {}
    explicit dict_value(float v)      : m_type(real_),   m_real(v) {}
    explicit dict_value(container::StringView v) : m_type(string), m_string(v) {}

    [[nodiscard]] type_ type() const noexcept { return m_type; }
    [[nodiscard]] bool is_valid() const noexcept { return m_type != none; }

    [[nodiscard]] bool as_bool() const { TD_ASSERT(m_type == boolean); return m_bool; }
    [[nodiscard]] int as_int() const { TD_ASSERT(m_type == integer); return m_int; }
    [[nodiscard]] float as_real() const { TD_ASSERT(m_type == real_); return m_real; }
    [[nodiscard]] const container::String& as_string() const { TD_ASSERT(m_type == string); return m_string; }

    bool try_get_bool(bool& out) const
    {
        if (m_type != boolean) return false;
        out = m_bool;
        return true;
    }

    bool try_get_int(int& out) const
    {
        if (m_type != integer) return false;
        out = m_int;
        return true;
    }

    bool try_get_real(float& out) const
    {
        if (m_type != real_) return false;
        out = m_real;
        return true;
    }

    bool try_get_string(container::String& out) const
    {
        if (m_type != string) return false;
        out = m_string;
        return true;
    }

    bool operator==(const dict_value& rhs) const
    {
        if (m_type != rhs.m_type) return false;
        switch (m_type)
        {
            case none:    return true;
            case boolean: return m_bool == rhs.m_bool;
            case integer: return m_int == rhs.m_int;
            case real_:   return m_real == rhs.m_real;
            case string:  return m_string == rhs.m_string;
            default:      return false;
        }
    }

    bool operator!=(const dict_value& rhs) const { return !(*this == rhs); }

private:
    type_ m_type;
    bool m_bool = false;
    int m_int = 0;
    float m_real = 0.0f;
    container::String m_string;
};

struct dict_entry
{
    string_id_type key = string_id_type::invalid;
    dict_value value;

    dict_entry() = default;
    dict_entry(string_id_type k, dict_value v) : key(k), value(std::move(v)) {}
};

class dict
{
public:
    static constexpr size_t MAX_PAIRS = 32767;

    dict() = default;

    explicit dict(size_t capacity)
    {
        m_entries.reserve(capacity);
        m_index.reserve(capacity);
    }

    dict(const dict&) = default;
    dict& operator=(const dict&) = default;
    dict(dict&&) noexcept = default;
    dict& operator=(dict&&) noexcept = default;

    void clear()
    {
        m_entries.clear();
        m_index.clear();
    }

    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    [[nodiscard]] dict_value::type_ get_type(string_id_type key) const
    {
        auto it = m_index.find(to_int(key));
        if (it == m_index.end()) return dict_value::none;
        return m_entries[it->second].value.type();
    }

    [[nodiscard]] bool known(string_id_type key, dict_value::type_ type) const
    {
        return get_type(key) == type;
    }

    void set_bool(string_id_type key, bool value) { set(key, dict_value(value)); }
    void set_int(string_id_type key, int value) { set(key, dict_value(value)); }
    void set_real(string_id_type key, float value) { set(key, dict_value(value)); }
    void set_string(string_id_type key, container::StringView value) { set(key, dict_value(value)); }

    [[nodiscard]] bool get_bool(string_id_type key, bool* exists = nullptr) const
    {
        const dict_value* v = find(key);
        if (v && v->type() == dict_value::boolean)
        {
            if (exists) *exists = true;
            return v->as_bool();
        }
        if (exists) *exists = false;
        return false;
    }

    [[nodiscard]] int get_int(string_id_type key, bool* exists = nullptr) const
    {
        const dict_value* v = find(key);
        if (v && v->type() == dict_value::integer)
        {
            if (exists) *exists = true;
            return v->as_int();
        }
        if (exists) *exists = false;
        return 0;
    }

    [[nodiscard]] float get_real(string_id_type key, bool* exists = nullptr) const
    {
        const dict_value* v = find(key);
        if (v && v->type() == dict_value::real_)
        {
            if (exists) *exists = true;
            return v->as_real();
        }
        if (exists) *exists = false;
        return 0.0f;
    }

    [[nodiscard]] container::String get_string(string_id_type key, bool* exists = nullptr) const
    {
        const dict_value* v = find(key);
        if (v && v->type() == dict_value::string)
        {
            if (exists) *exists = true;
            return v->as_string();
        }
        if (exists) *exists = false;
        return {};
    }

    [[nodiscard]] string_id_type nth_key(size_t n) const
    {
        if (n >= m_entries.size()) return string_id_type::invalid;
        return m_entries[n].key;
    }

    [[nodiscard]] const dict_value* nth_value(size_t n) const
    {
        if (n >= m_entries.size()) return nullptr;
        return &m_entries[n].value;
    }

    void remove(string_id_type key)
    {
        auto it = m_index.find(to_int(key));
        if (it == m_index.end()) return;

        size_t idx = it->second;
        size_t last = m_entries.size() - 1;

        if (idx != last)
        {
            m_entries[idx] = std::move(m_entries[last]);
            m_index[to_int(m_entries[idx].key)] = static_cast<uint32_t>(idx);
        }

        m_entries.pop_back();
        m_index.erase(it);
    }

    bool operator==(const dict& rhs) const
    {
        return m_entries == rhs.m_entries;
    }

    bool operator!=(const dict& rhs) const { return !(*this == rhs); }

private:
    container::Vector<dict_entry> m_entries;
    HashMap<uint32_t, uint32_t> m_index;

    void set(string_id_type key, dict_value value)
    {
        auto it = m_index.find(to_int(key));
        if (it != m_index.end())
        {
            m_entries[it->second].value = std::move(value);
            return;
        }

        if (m_entries.size() >= MAX_PAIRS) return;

        uint32_t idx = static_cast<uint32_t>(m_entries.size());
        m_index[to_int(key)] = idx;
        m_entries.emplace_back(key, std::move(value));
    }

    const dict_value* find(string_id_type key) const
    {
        auto it = m_index.find(to_int(key));
        if (it == m_index.end()) return nullptr;
        return &m_entries[it->second].value;
    }
};

using DictValue = dict_value;
using DictEntry = dict_entry;
using Dict = dict;

} // namespace container

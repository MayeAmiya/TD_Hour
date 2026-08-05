#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "debug/td_assert.h"
#include "hash_containers.h"
#include "string_utils.h"

namespace container {

enum class string_id_type : uint32_t
{
    invalid  = 0,
    max_value = (1u << 23) - 1,
};

using string_id = string_id_type;

[[nodiscard]] constexpr uint32_t to_int(string_id_type id) noexcept
{
    return static_cast<uint32_t>(id);
}

[[nodiscard]] constexpr string_id_type with_string_id(uint32_t val) noexcept
{
    TD_ASSERT(val <= to_int(string_id_type::max_value));
    return val <= to_int(string_id_type::max_value)
        ? static_cast<string_id_type>(val)
        : string_id_type::invalid;
}

class string_id_generator
{
public:
    string_id_generator()
        : m_next_id(1)
    {
        m_id_to_name.reserve(4096);
        m_id_to_name.push_back("");
    }

    [[nodiscard]] string_id_type name_to_id(container::StringView name)
    {
        return name_to_id_impl(name, false);
    }

    [[nodiscard]] string_id_type name_to_lowercase_id(container::StringView name)
    {
        return name_to_id_impl(name, true);
    }

    // Canonical registration is the deterministic content-load path. Input
    // order does not affect assigned IDs; mutation remains bound to the first
    // registration thread until freeze().
    [[nodiscard]] bool register_names(
        container::Span<const container::StringView> names,
        bool lowercase = false)
    {
        struct Candidate final {
            container::String key;
            container::String display;
        };
        container::Vector<Candidate> candidates;
        candidates.reserve(names.size());
        for (const container::StringView name : names) {
            if (name.empty()) continue;
            Candidate candidate{.key = container::String{name},
                                .display = container::String{name}};
            if (lowercase) {
                for (char& value : candidate.key)
                    value = asciiLower(value);
            }
            candidates.push_back(std::move(candidate));
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                return left.key != right.key ? left.key < right.key
                                             : left.display < right.display;
            });
        candidates.erase(std::unique(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                return left.key == right.key;
            }), candidates.end());

        const std::scoped_lock lock(m_mutex);
        if (m_frozen || !claim_registration_thread_locked()) return false;
        size_t newCount = 0;
        for (const Candidate& candidate : candidates) {
            if (m_name_to_id.find(candidate.key) == m_name_to_id.end())
                ++newCount;
        }
        const uint32_t maximum = to_int(string_id_type::max_value);
        if (newCount > static_cast<size_t>(maximum - (m_next_id - 1u)))
            return false;
        m_name_to_id.reserve(m_name_to_id.size() + newCount);
        m_id_to_name.reserve(m_id_to_name.size() + newCount);
        for (Candidate& candidate : candidates) {
            if (m_name_to_id.find(candidate.key) != m_name_to_id.end())
                continue;
            const uint32_t id = m_next_id++;
            m_name_to_id.emplace(std::move(candidate.key), id);
            m_id_to_name.push_back(std::move(candidate.display));
        }
        return true;
    }

    [[nodiscard]] bool freeze()
    {
        const std::scoped_lock lock(m_mutex);
        if (!claim_registration_thread_locked()) return false;
        m_frozen = true;
        return true;
    }

    [[nodiscard]] bool is_frozen() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_frozen;
    }

    [[nodiscard]] string_id_type find_id(
        container::StringView name, bool lowercase = false) const
    {
        if (name.empty()) return string_id_type::invalid;
        container::String key{name};
        if (lowercase) {
            for (char& value : key) value = asciiLower(value);
        }
        const std::scoped_lock lock(m_mutex);
        const auto found = m_name_to_id.find(key);
        return found == m_name_to_id.end()
            ? string_id_type::invalid
            : with_string_id(found->second);
    }

    [[nodiscard]] container::String id_to_name(string_id_type id) const
    {
        const std::scoped_lock lock(m_mutex);
        const uint32_t idx = to_int(id);
        if (idx == 0 || idx >= m_id_to_name.size())
        {
            return {};
        }
        return m_id_to_name[idx];
    }

    [[nodiscard]] bool contains(container::StringView name) const
    {
        return find_id(name) != string_id_type::invalid;
    }

    [[nodiscard]] size_t size() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_name_to_id.size();
    }

    [[nodiscard]] bool clear()
    {
        const std::scoped_lock lock(m_mutex);
        if (m_frozen ||
            (m_has_registration_thread &&
             m_registration_thread != std::this_thread::get_id())) {
            return false;
        }
        m_name_to_id.clear();
        m_id_to_name.clear();
        m_id_to_name.push_back("");
        m_next_id = 1;
        m_frozen = false;
        m_has_registration_thread = false;
        m_registration_thread = {};
        return true;
    }

private:
    [[nodiscard]] bool claim_registration_thread_locked()
    {
        const std::thread::id current = std::this_thread::get_id();
        if (!m_has_registration_thread) {
            m_registration_thread = current;
            m_has_registration_thread = true;
            return true;
        }
        return m_registration_thread == current;
    }

    [[nodiscard]] string_id_type name_to_id_impl(
        container::StringView name, bool lowercase)
    {
        if (name.empty()) return string_id_type::invalid;
        container::String key{name};
        if (lowercase) {
            for (char& value : key) value = asciiLower(value);
        }
        const std::scoped_lock lock(m_mutex);
        const auto found = m_name_to_id.find(key);
        if (found != m_name_to_id.end()) return with_string_id(found->second);
        if (m_frozen || !claim_registration_thread_locked() ||
            m_next_id > to_int(string_id_type::max_value)) {
            return string_id_type::invalid;
        }
        const uint32_t id = m_next_id++;
        m_name_to_id.emplace(std::move(key), id);
        m_id_to_name.emplace_back(name);
        return with_string_id(id);
    }

    mutable std::mutex m_mutex;
    HashMap<container::String, uint32_t> m_name_to_id;
    container::Vector<container::String> m_id_to_name;
    uint32_t m_next_id;
    bool m_frozen = false;
    bool m_has_registration_thread = false;
    std::thread::id m_registration_thread{};
};

inline string_id_generator& get_string_id_generator()
{
    static string_id_generator instance;
    return instance;
}

inline string_id_type SID(container::StringView name)
{
    return get_string_id_generator().name_to_id(name);
}

inline string_id_type SID_LOWER(container::StringView name)
{
    return get_string_id_generator().name_to_lowercase_id(name);
}

class static_string_id
{
public:
    explicit static_string_id(const char* name)
        : m_id(0), m_name(name)
    {
    }

    [[nodiscard]] string_id_type id() const
    {
        uint32_t cached = m_id.load(std::memory_order_acquire);
        if (cached != 0 || !m_name) return with_string_id(cached);
        const string_id_type resolved =
            get_string_id_generator().find_id(m_name);
        const uint32_t value = to_int(resolved);
        if (value == 0) return string_id_type::invalid;
        uint32_t expected = 0;
        static_cast<void>(m_id.compare_exchange_strong(
            expected, value, std::memory_order_release,
            std::memory_order_relaxed));
        return with_string_id(m_id.load(std::memory_order_acquire));
    }

    operator string_id_type() const { return id(); }

    [[nodiscard]] const char* name() const { return m_name; }

private:
    mutable std::atomic<uint32_t> m_id;
    const char* m_name;
};

struct string_id_hash
{
    size_t operator()(string_id_type id) const noexcept
    {
        return std::hash<uint32_t>()(to_int(id));
    }
};

using StringIdType = string_id_type;
using StringId = string_id;
using StringIdGenerator = string_id_generator;
using StringIdHash = string_id_hash;
using StaticStringId = static_string_id;

} // namespace container

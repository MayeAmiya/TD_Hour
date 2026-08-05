#pragma once

#include "container/container_types.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
namespace container {

// A small, cache-friendly set for stable simulation identifiers.  Its
// iteration order is part of the contract: values are always ascending under
// Compare, regardless of insertion/removal history.  This is deliberately not
// a hash set: callers can safely use it for canonical serialization, CRC and
// deterministic gameplay iteration.
//
// Insert/erase are O(n), which is the right first trade-off for player-owned
// object IDs and script-team membership: membership transitions are far less
// frequent than iteration, and correctness is more valuable than a hidden
// unordered iteration dependency.  If profiling later identifies a hotspot,
// an acceleration index may be added behind this same ordered API.
template <typename Id, typename Compare = std::less<Id>>
class ordered_id_set final {
public:
    using value_type = Id;
    using size_type = std::size_t;
    using const_iterator = typename container::Vector<Id>::const_iterator;

    ordered_id_set() = default;
    explicit ordered_id_set(Compare compare) : m_compare(std::move(compare)) {}

    [[nodiscard]] bool empty() const noexcept { return m_values.empty(); }
    [[nodiscard]] size_type size() const noexcept { return m_values.size(); }
    [[nodiscard]] size_type capacity() const noexcept { return m_values.capacity(); }

    void reserve(size_type capacity) { m_values.reserve(capacity); }
    void clear() noexcept { m_values.clear(); }

    [[nodiscard]] bool contains(const Id& value) const {
        const auto found = lowerBound(value);
        return found != m_values.end() && equivalent(*found, value);
    }

    // Returns true only when a new value was inserted.
    bool insert(const Id& value) {
        // Lifecycle-created ObjectIds are monotonically increasing.  Map
        // import and ordinary production therefore append for the common
        // case, preserving the same canonical order without repeatedly
        // shifting a ScriptTeam/player-owned vector.  Transfers of an older
        // object still take the generic ordered path below.
        if (m_values.empty() || m_compare(m_values.back(), value)) {
            m_values.push_back(value);
            return true;
        }
        const auto found = lowerBound(value);
        if (found != m_values.end() && equivalent(*found, value)) return false;
        m_values.insert(found, value);
        return true;
    }

    bool insert(Id&& value) {
        if (m_values.empty() || m_compare(m_values.back(), value)) {
            m_values.push_back(std::move(value));
            return true;
        }
        const auto found = lowerBound(value);
        if (found != m_values.end() && equivalent(*found, value)) return false;
        m_values.insert(found, std::move(value));
        return true;
    }

    // Returns true only when an existing value was removed.
    bool erase(const Id& value) {
        const auto found = lowerBound(value);
        if (found == m_values.end() || !equivalent(*found, value)) return false;
        m_values.erase(found);
        return true;
    }

    // Replaces content from arbitrary input while preserving the canonical
    // order and removing duplicates.  This is useful after a save/load rebuild
    // and makes the result independent from source iteration order.
    void assign(container::Span<const Id> values) {
        m_values.assign(values.begin(), values.end());
        normalize();
    }

    void assign(container::Vector<Id> values) {
        m_values = std::move(values);
        normalize();
    }

    [[nodiscard]] container::Span<const Id> values() const noexcept { return m_values; }
    [[nodiscard]] const_iterator begin() const noexcept { return m_values.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_values.end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return m_values.cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return m_values.cend(); }

private:
    [[nodiscard]] const_iterator lowerBound(const Id& value) const {
        return std::lower_bound(m_values.begin(), m_values.end(), value, m_compare);
    }

    [[nodiscard]] bool equivalent(const Id& lhs, const Id& rhs) const {
        return !m_compare(lhs, rhs) && !m_compare(rhs, lhs);
    }

    void normalize() {
        std::sort(m_values.begin(), m_values.end(), m_compare);
        m_values.erase(std::unique(m_values.begin(), m_values.end(),
            [this](const Id& lhs, const Id& rhs) { return equivalent(lhs, rhs); }),
            m_values.end());
    }

    container::Vector<Id> m_values;
    [[no_unique_address]] Compare m_compare{};
};

template<typename Id, typename Compare = std::less<Id>>
using OrderedIdSet = ordered_id_set<Id, Compare>;

} // namespace container

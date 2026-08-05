#pragma once

#include <cstdint>
#include <compare>
#include <functional>
#include <limits>
#include <optional>

namespace engine {

struct ObjectId {
    uint32_t value = 0;

    constexpr bool isValid() const { return value != 0; }
    explicit constexpr operator bool() const { return isValid(); }

    constexpr auto operator<=>(const ObjectId&) const noexcept = default;
};

inline constexpr ObjectId INVALID_OBJECT_ID{};

class ObjectIdAllocator {
public:
    // ID zero is permanently invalid.  Exhaustion must be observable rather
    // than wrapping to zero or silently reusing a prior object's identity.
    [[nodiscard]] std::optional<ObjectId> tryAllocate() noexcept {
        if (m_next == 0) return std::nullopt;
        const ObjectId result{m_next};
        if (m_next == std::numeric_limits<uint32_t>::max()) {
            m_next = 0;
        } else {
            ++m_next;
        }
        return result;
    }

    // Compatibility bridge for the pre-existing GameSession creation API.
    // New deterministic lifecycle code should use tryAllocate() and surface a
    // creation error if the identity space is exhausted.
    [[nodiscard]] ObjectId allocate() noexcept {
        return tryAllocate().value_or(INVALID_OBJECT_ID);
    }

    void reset() {
        m_next = 1;
    }

    [[nodiscard]] bool exhausted() const noexcept { return m_next == 0; }
    [[nodiscard]] uint32_t nextValueForSnapshot() const noexcept { return m_next; }

    // Restore only canonical allocator state: zero means exhausted, while
    // every nonzero value is the next never-before-issued ID.
    void restoreNextValue(uint32_t next) noexcept {
        m_next = next;
    }

private:
    uint32_t m_next = 1;
};

} // namespace engine

template <>
struct std::hash<engine::ObjectId> {
    size_t operator()(engine::ObjectId id) const noexcept {
        return std::hash<uint32_t>{}(id.value);
    }
};

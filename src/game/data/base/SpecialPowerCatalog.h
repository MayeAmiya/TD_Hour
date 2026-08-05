#pragma once

#include "core/container/container_types.h"
#include "game/data/base/SpecialPowerType.h"

#include "math/fixed/q32_32.h"

#include <compare>
#include <cstdint>

namespace engine {

// Stable identity inside one sealed game-content snapshot. Zero is always the
// invalid value; IDs are assigned by exact, case-sensitive authored name order
// and therefore do not depend on archive/layer enumeration order.
struct SpecialPowerContentId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    constexpr auto operator<=>(const SpecialPowerContentId&) const noexcept = default;
};

inline constexpr SpecialPowerContentId INVALID_SPECIAL_POWER_CONTENT_ID{};

// Immutable modern projection of RefCode SpecialPowerTemplate. Durations stay
// in authored milliseconds until a confirmed session converts them to ticks;
// distances are frozen as deterministic Q32.32 values at content-load time.
struct SpecialPowerDefinition final {
    SpecialPowerContentId id = INVALID_SPECIAL_POWER_CONTENT_ID;
    container::String name;

    uint32_t reloadTimeMilliseconds = 0;
    container::String requiredScience;
    bool publicTimer = false;
    // RefCode SpecialPowerTemplate::m_type. Loaded from INI Enum; Invalid means
    // the field was omitted or left as SPECIAL_INVALID.
    game::SpecialPowerType specialPowerType = game::SpecialPowerType::Invalid;
    uint32_t detectionTimeMilliseconds = 10'000;
    bool sharedSyncedTimer = false;
    uint32_t viewObjectDurationMilliseconds = 0;
    math::q32_32 viewObjectRange;
    math::q32_32 radiusCursorRadius;
    bool shortcutPower = false;
    container::String academyClassification;

    // Presentation metadata. These strings intentionally do not participate
    // in the simulation fingerprint.
    container::String initiateSound;
    container::String initiateAtLocationSound;
};

// Sealed SpecialPower.ini catalog. It preserves RefCode's DefaultSpecialPower
// copy-on-first-definition and partial override behavior without retaining the
// legacy mutable global store or its load-order-assigned identifiers.
class SpecialPowerCatalog final {
public:
    [[nodiscard]] static container::Vector<container::String>
    enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots);

    [[nodiscard]] bool loadFromVfs(container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path, container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                        container::String* error = nullptr);
    [[nodiscard]] bool loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                                  container::String* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] uint64_t simulationFingerprint() const noexcept {
        return m_simulationFingerprint;
    }
    [[nodiscard]] const SpecialPowerDefinition* find(container::StringView name) const;
    [[nodiscard]] const SpecialPowerDefinition* find(SpecialPowerContentId id) const noexcept;
    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }

private:
    struct Entry final {
        container::String key;
        SpecialPowerDefinition definition;
    };

    [[nodiscard]] static uint64_t calculateFingerprint(const container::Vector<Entry>& entries);
    [[nodiscard]] bool loadFromVfsFilesImpl(
        const container::Vector<container::String>& logicalFiles,
        bool resetCatalog, container::String* error);

    container::Vector<Entry> m_entries;
    uint64_t m_simulationFingerprint = 0;
    bool m_loaded = false;
};

} // namespace engine

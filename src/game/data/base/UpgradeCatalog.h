#pragma once

#include "core/container/bit_flags.h"
#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <compare>
#include <cstdint>
namespace engine {

// Stable session-content identity for an Upgrade definition. It is deliberately
// separate from an ECS entity and from the legacy name-key bit field: names are
// authoring/UI input, while a queued production entry stores this compact ID.
// IDs are 1-based sealed-catalog ordinals (bit index = value - 1).
struct UpgradeContentId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    constexpr auto operator<=>(const UpgradeContentId&) const noexcept = default;
};

inline constexpr UpgradeContentId INVALID_UPGRADE_CONTENT_ID{};

// Matches RefCode UpgradeMaskType / UPGRADE_MAX_COUNT. ContentId bit packing
// requires catalog.size() <= kUpgradeMaskBits.
inline constexpr size_t kUpgradeMaskBits = 512;
using UpgradeMask = container::BitFlags<kUpgradeMaskBits>;

[[nodiscard]] constexpr bool upgradeIdInMaskRange(
    UpgradeContentId id) noexcept {
    return id && id.value <= static_cast<uint32_t>(kUpgradeMaskBits);
}

[[nodiscard]] constexpr size_t upgradeBitIndex(UpgradeContentId id) noexcept {
    return static_cast<size_t>(id.value - 1u);
}

[[nodiscard]] inline bool upgradeMaskTest(
    const UpgradeMask& mask, UpgradeContentId id) noexcept {
    return upgradeIdInMaskRange(id) && mask.test(upgradeBitIndex(id));
}

inline void upgradeMaskSet(UpgradeMask& mask, UpgradeContentId id) noexcept {
    if (upgradeIdInMaskRange(id)) mask.set(upgradeBitIndex(id));
}

inline void upgradeMaskClear(UpgradeMask& mask, UpgradeContentId id) noexcept {
    if (upgradeIdInMaskRange(id)) mask.reset(upgradeBitIndex(id));
}

class UpgradeCatalog;

// Engine-hardcoded upgrade identities (RefCode-aligned exact names).
// Content-identity constants for find()/mask resolution — not a closed enum of
// every Upgrade.ini entry. New data-driven upgrades need only the catalog.
namespace well_known_upgrade {
inline constexpr container::StringView Nationalism = "Upgrade_Nationalism";
inline constexpr container::StringView Fanaticism = "Upgrade_Fanaticism";
inline constexpr container::StringView GlaWorkerShoes = "Upgrade_GLAWorkerShoes";
inline constexpr container::StringView AmericaSupplyLines =
    "Upgrade_AmericaSupplyLines";
inline constexpr container::StringView ChinaEmpMines = "Upgrade_ChinaEMPMines";
inline constexpr container::StringView AmericaChemicalSuits =
    "Upgrade_AmericaChemicalSuits";
inline constexpr container::StringView VeterancyVeteran =
    "Upgrade_Veterancy_VETERAN";
inline constexpr container::StringView VeterancyElite =
    "Upgrade_Veterancy_ELITE";
inline constexpr container::StringView VeterancyHeroic =
    "Upgrade_Veterancy_HEROIC";
} // namespace well_known_upgrade

enum class UpgradeDefinitionType : uint8_t {
    Player,
    Object,
};

// Immutable simulation/presentation projection of one legacy UpgradeTemplate.
// Build time is frozen as Q32.32 authored seconds; a production transaction
// converts it to the session's confirmed frame rate without consulting a
// mutable GlobalData or UpgradeCenter singleton.
struct UpgradeDefinition final {
    UpgradeContentId id = INVALID_UPGRADE_CONTENT_ID;
    container::String name;
    UpgradeDefinitionType type = UpgradeDefinitionType::Player;
    math::q32_32 buildTimeSeconds;
    int64_t buildCost = 0;

    // Retained for the later UI/audio presentation boundary. They do not take
    // part in the simulation fingerprint or confirmed production logic.
    container::String displayNameLabel;
    container::String buttonImage;
    container::String researchCompleteSound;
    container::String unitSpecificSound;
    container::String academyClassification;
};

// A sealed Upgrade.ini catalog. It follows the legacy `DefaultUpgrade` copy
// model while replacing global linked templates and mask bits with a sorted,
// immutable value table and session-stable IDs. Upgrade names intentionally
// remain exact-case identity, matching RefCode NameKey lookup.
class UpgradeCatalog final {
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
    [[nodiscard]] const UpgradeDefinition* find(container::StringView name) const;
    [[nodiscard]] const UpgradeDefinition* find(UpgradeContentId id) const noexcept;
    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }

private:
    struct Entry final {
        container::String key;
        UpgradeDefinition definition;
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

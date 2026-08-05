#pragma once

#include "core/container/container_types.h"
#include <cstdint>

namespace engine {

// Session-frozen subset of GlobalData used by authoritative sell settlement.
struct EconomySimulationRules final {
    // GlobalData::ValuePerSupplyBox.  It is frozen with the session so supply
    // transfer, scripts and replay never consult mutable process globals.
    int64_t valuePerSupplyBox = 100;
    // GlobalData's constructor default is 100%; shipped GameData normally
    // replaces it. Keep an integer rational so common authored percentages
    // such as 70% settle exactly (Q32.32 would truncate 1000 * 0.7 to 699).
    // Values above 100% remain legal for legacy mods.
    int64_t sellPercentageNumerator = 1;
    uint64_t sellPercentageDenominator = 1;

    void canonicalize() noexcept;
    [[nodiscard]] int64_t applySellPercentage(int64_t value) const noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    [[nodiscard]] static bool loadFromLegacyGameData(
        container::StringView path, EconomySimulationRules& rules,
        container::String* error = nullptr);
};

} // namespace engine

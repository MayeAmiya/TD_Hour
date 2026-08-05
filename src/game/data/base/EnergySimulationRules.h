#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstdint>
namespace engine {

// Immutable-at-session-start projection of the GlobalData values used by
// ThingTemplate::calcTimeToBuild for power and multiple-factory speed. Keeping
// these as Q32.32 values prevents a mutable config singleton or a platform
// floating-point division from deciding a confirmed production deadline.
struct EnergySimulationRules final {
    using Scalar = math::q32_32;

    // Match RefCode GlobalData's constructor defaults. Shipped GameData
    // normally supplies these values; a sparse/mod fixture that omits them
    // therefore retains the original zero-valued behavior rather than a
    // convenient but incompatible modern fallback.
    static constexpr double kDefaultMinimumLowEnergyProductionSpeed = 0.0;
    static constexpr double kDefaultMaximumLowEnergyProductionSpeed = 0.0;
    static constexpr double kDefaultLowEnergyPenaltyModifier = 0.0;
    static constexpr double kDefaultMultipleFactoryMultiplier = 0.0;
    static constexpr double kFallbackPositiveProductionSpeed = 0.01;

    Scalar minimumLowEnergyProductionSpeed{
        kDefaultMinimumLowEnergyProductionSpeed};
    Scalar maximumLowEnergyProductionSpeed{
        kDefaultMaximumLowEnergyProductionSpeed};
    Scalar lowEnergyPenaltyModifier{
        kDefaultLowEnergyPenaltyModifier};
    // RefCode's GameData.MultipleFactory is applied once per additional
    // existing facility, and only to APPEARS_AT_RALLY_POINT products. Zero
    // disables the bonus exactly like the legacy GlobalData default.
    Scalar multipleFactoryMultiplier{
        kDefaultMultipleFactoryMultiplier};

    // Authored values remain intact. RefCode applies max() then (only while
    // underpowered) min(), and consumers own any necessary safe derived rate.
    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Returns the exact fixed-point speed factor used by the current
    // production slice. Production/consumption are non-negative canonical
    // player aggregates; a sabotage flag makes production zero for this tick.
    [[nodiscard]] Scalar productionSpeed(int32_t production, int32_t consumption,
                                         bool powerSabotaged) const noexcept;

    // Applies the source's positive `buildTime /= penaltyRate` conversion to
    // an already faction-modified frame count.  The result is floored like a
    // legacy positive Int cast and is always at least one logic frame.
    [[nodiscard]] uint32_t adjustBuildFrames(uint32_t baseFrames,
                                             int32_t production,
                                             int32_t consumption,
                                             bool powerSabotaged) const noexcept;

    [[nodiscard]] uint32_t adjustForMultipleFactories(
        uint32_t baseFrames, uint32_t facilityCount) const noexcept;

    // Reads the relevant GameData keys through the same layered legacy INI
    // path as the other simulation rule compilers. Power contributors and
    // build-facility ownership remain live ECS/player data.
    [[nodiscard]] static bool loadFromLegacyGameData(container::StringView path,
                                                      EnergySimulationRules& rules,
                                                      container::String* error = nullptr);
};

} // namespace engine

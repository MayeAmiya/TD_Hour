#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"
namespace engine {

// Immutable-at-session-start projection of the legacy GameData physics
// globals.  Gravity deliberately stays in modern world-units/second²: the
// original INI parser converted this authoring unit to per-30-Hz-frame²,
// while the ECS integrator consumes the supplied fixed delta directly.
struct PhysicsSimulationRules final {
    static constexpr float kLegacyLogicFramesPerSecond = 30.0f;
    static constexpr float kLegacyPerFrameSquaredToPerSecondSquared =
        kLegacyLogicFramesPerSecond * kLegacyLogicFramesPerSecond;
    static constexpr float kDefaultGravityUnitsPerSecondSq =
        -kLegacyPerFrameSquaredToPerSecondSquared;
    static constexpr float kDefaultGroundStiffness = 0.5f;
    static constexpr float kDefaultStructureStiffness = 0.5f;
    static constexpr float kDefaultStructureRubbleHeight = 1.0f;
    static constexpr float kMinimumGroundStiffness = 0.01f;
    static constexpr float kMaximumGroundStiffness = 0.99f;

    // The parser may use float temporaries, but the winning content record is
    // quantized before it leaves the loader. Simulation/session code never
    // converts these authoritative values at the point of use.
    math::q32_32 gravityUnitsPerSecondSq{kDefaultGravityUnitsPerSecondSq};
    math::q32_32 groundStiffness{kDefaultGroundStiffness};
    math::q32_32 structureStiffness{kDefaultStructureStiffness};
    math::q32_32 defaultStructureRubbleHeight{
        kDefaultStructureRubbleHeight};

    // RefCode applies stiffness bounds at bounce time. Preserve the authored
    // value here; only representation-equivalent normalization belongs in
    // canonicalize().
    void canonicalize() noexcept;

    // Applies a Map.ini/solo.ini modifier to an already frozen base value.
    // Only authored GameData fields are changed; failure leaves this object
    // untouched.
    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Loads the winning VFS instance of a legacy `GameData ... End` source.
    // Only gameplay physics keys are consumed here; unrelated GlobalData
    // fields remain outside the current ECS migration slice.
    [[nodiscard]] static bool loadFromLegacyGameData(container::StringView path,
                                                      PhysicsSimulationRules& rules,
                                                      container::String* error = nullptr);
};

} // namespace engine

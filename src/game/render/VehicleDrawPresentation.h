#pragma once

#include <cstdint>
#include <optional>

#include "core/ecs/registry.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/terrain/TerrainLogic.h"

namespace engine
{

struct ObjectLocomotionComponent;
struct ObjectPhysicsComponent;

struct VehicleMotiveSample final
{
    float planarSpeedUnitsPerSecond = 0.0f;
    bool moving = false;
};

// RefCode vehicle Draw modules consume PhysicsBehavior velocity/motive state.
// TD's transform-authoritative locomotor does not mirror ordinary movement
// into Physics velocity, while impulses and collision motion may bypass the
// locomotor. Prefer a real Physics planar velocity and fall back to confirmed
// locomotor motive/speed only when Physics publishes no planar motion. Dust,
// tread debris and TrackMarks must all consume this same sample.
[[nodiscard]] VehicleMotiveSample sampleVehicleMotive(
    const ObjectPhysicsComponent* physics,
    const ObjectLocomotionComponent* locomotion) noexcept;

struct VehicleDrawConfirmedInput final
{
    float deltaSeconds = 1.0f / 30.0f;
    float yawRadians = 0.0f;
    float planarSpeedUnitsPerSecond = 0.0f;
    float maximumSpeedUnitsPerSecond = 0.0f;
    float planarAccelerationUnitsPerSecondSq = 0.0f;
    float velocityAccelerationDot = 0.0f;
    float frontWheelTurnAngleRadians = 0.0f;
    std::optional<float> relativeGoalAngleRadians;
    uint64_t confirmedTick = 0;
    bool moving = false;
    bool movingBackward = false;
    bool grounded = true;
    bool hidden = false;
};

// RefCode compares 0.01 against acceleration expressed per 30 Hz logic
// frame. Convert it once to the modern seconds-based locomotor convention.
inline constexpr float kVehicleDrawAccelerationThresholdPerSecondSq = 9.0f;
inline constexpr float kVehicleDrawTurningEpsilon = 0.00001f;
inline constexpr float kVehicleDrawDebrisSpeedSquaredThreshold = 0.00001f;
inline constexpr float kVehicleDrawDustSizeCap = 2.0f;
inline constexpr float kVehicleDrawWheelSteeringSmoothness = 10.0f;

void advanceVehicleDrawChannel(const game::VehicleDrawVisualRecipe& recipe,
                               const VehicleDrawConfirmedInput& input,
                               VehicleDrawChannelPresentationState& state) noexcept;

void updateVehicleDrawPresentation(
    ecs::registry& registry, const game::terrain::TerrainLogic& terrain,
    float deltaSeconds, uint64_t confirmedTick) noexcept;

} // namespace engine

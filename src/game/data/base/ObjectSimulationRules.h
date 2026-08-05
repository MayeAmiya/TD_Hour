#pragma once

#include "game/data/base/AISimulationRules.h"
#include "game/data/base/BaseRegenerationRules.h"
#include "game/data/base/BuildPlacementSimulationRules.h"
#include "game/data/base/DifficultySimulationRules.h"
#include "game/data/base/EconomySimulationRules.h"
#include "game/data/base/EnergySimulationRules.h"
#include "game/data/base/PhysicsSimulationRules.h"
#include "game/data/base/VeterancySimulationRules.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

// Ordered exactly like RefCode's BodyDamageType.
enum class ObjectBodyDamageState : uint8_t {
    Pristine,
    Damaged,
    ReallyDamaged,
    Rubble,
};

// Session-frozen projection of the original GlobalData values used by
// authoritative object systems. Confirmed frames never observe the mutable
// loader or the legacy process-global singleton.
struct ObjectSimulationRules final {
    math::q32_32 unitDamagedThresholdFixed =
        math::q32_32{-1};
    math::q32_32 unitReallyDamagedThresholdFixed =
        math::q32_32{-1};
    ObjectBodyDamageState movementPenaltyDamageState =
        ObjectBodyDamageState::ReallyDamaged;
    uint32_t logicFramesPerSecond =
        static_cast<uint32_t>(PhysicsSimulationRules::kLegacyLogicFramesPerSecond);
    math::q32_32 logicDeltaSeconds = math::q32_32::from_fraction(1, 30);
    math::q32_32 gravityUnitsPerSecondSq{
        PhysicsSimulationRules::kDefaultGravityUnitsPerSecondSq};
    math::q32_32 groundStiffness{
        PhysicsSimulationRules::kDefaultGroundStiffness};
    math::q32_32 structureStiffness{
        PhysicsSimulationRules::kDefaultStructureStiffness};
    math::q32_32 defaultStructureRubbleHeight{
        PhysicsSimulationRules::kDefaultStructureRubbleHeight};
    uint32_t maxTunnelCapacity = 10;
    math::q32_32 standardMinefieldDistance{40.0f};
    math::q32_32 standardMinefieldDensity{0.01f};
    // Stock ZH GameData.GroupMoveClickToGatherAreaFactor. Captured at session
    // start so group-order routing never reads mutable process globals.
    math::q32_32 groupMoveClickToGatherFactor{1.0f};
    // GameData.SpecialPowerViewObject (RefCode GlobalData
    // m_specialPowerViewObjectName, stock value `SuperweaponPing`). This is
    // simulation state: SpecialPowerModule::createViewObject spawns the named
    // object at the power's target so its ShroudClearingRange reveals the
    // impact area. Frozen at session start; an empty name disables it.
    container::String specialPowerViewObject;
    bool preserveTunnelHealStacking = false;
    AISimulationRules ai;
    BaseRegenerationRules baseRegeneration;
    BuildPlacementSimulationRules buildPlacement;
    EnergySimulationRules energy;
    EconomySimulationRules economy;
    DifficultySimulationRules difficulty;
    VeterancySimulationRules veterancy;
    PhysicsSimulationRules physics;

    // Patches the GameData fields represented by this aggregate and all of
    // its modern typed child rules. The operation commits only after every
    // child parser accepts the modifier.
    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    [[nodiscard]] bool applyLegacyAIDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);
};

} // namespace engine

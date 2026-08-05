#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include "game/object/plan/status/ObjectCrateCollidePlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;
struct ObjectSimulationRules;

struct ObjectCrateCollideComponent final {
    container::SharedPtr<const game::ObjectCrateCollidePlan> plan;
};

enum class ObjectIntentionalContactKind : uint8_t {
    HijackVehicle,
    ConvertToCarBomb,
    SabotageBuilding,
};

// Read-only ActionManager equivalent used at player-command admission.  The
// collision system remains the only effect owner and repeats this validation
// against the final confirmed overlap.
[[nodiscard]] bool canObjectPerformIntentionalCrateContact(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, ObjectId source, ObjectId target,
    ObjectIntentionalContactKind kind) noexcept;

// Preserve stock authored ordering where a consuming mobile-crate behavior
// executes before SquishCollide (notably Pilot and ConvertToCarBomb).
[[nodiscard]] bool hasEarlierConsumingCrateCollisionPriority(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, ObjectId source, ecs::entity sourceEntity,
    ObjectId target, ecs::entity targetEntity,
    uint32_t laterAuthoredOrder,
    math::q32_32 significantAboveTerrainHeight) noexcept;

// Read-only eligibility used by AI target selection.  This deliberately
// exposes only the ordinary autonomous-pickup subset of CrateCollide: pilot,
// hijack, car-bomb conversion and sabotage remain explicit-order contacts.
// The collision system still owns the actual effect and final overlap check.
[[nodiscard]] bool canObjectAIAutonomouslyPickUpCrate(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    ObjectId picker, ObjectId crate) noexcept;

// Value-only command emitted after movement/physics has produced the current
// confirmed transforms. GameSession applies the cross-domain effect through
// PlayerRegistry, ObjectSimulation, MapVisibilityAuthority or the central
// spawn transaction, then retains the same value as an observable event.
struct ObjectCratePickupCommand final {
    game::ObjectCrateCollideKind kind = game::ObjectCrateCollideKind::Heal;
    ObjectId crate = INVALID_OBJECT_ID;
    ObjectId picker = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
    PlayerId victimPlayer = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    int64_t moneyAmount = 0;
    uint32_t unitCount = 0;
    container::String unitName;
    uint32_t veterancyLevelsToGain = 0;
    uint32_t veterancyEffectRange = 0;
    bool veterancyIsPilot = false;
    math::q32_32 salvageWeaponChance{int32_t{1}};
    math::q32_32 salvageLevelChance =
        math::q32_32::from_fraction(1, 4);
    math::q32_32 salvageMoneyChance =
        math::q32_32::from_fraction(3, 4);
    int32_t salvageMinimumMoney = 25;
    int32_t salvageMaximumMoney = 75;
    uint32_t sabotageDurationMilliseconds = 0;
    uint32_t stealCashAmount = 0;
    game::ObjectSalvageCrateReward salvageReward =
        game::ObjectSalvageCrateReward::None;
    // Confirmed authoritative command payload. Presentation consumers project
    // it to float only at their boundary.
    LogicFixedVec3 cratePosition{};
    LogicFixedVec3 position{};
    math::q32_32 rotationRadians{};
    container::String executeFx;
    container::String executeAnimation;
    container::String pickupAudio;
    container::String convertFxList;
    // Authored floats are quantized once when the command enters runtime.
    math::q32_32 executeAnimationTimeSeconds{};
    math::q32_32 executeAnimationZRisePerSecond{};
    bool executeAnimationFades = true;
    bool allowMultiPickup = false;
    // Some legacy mobile-crate behaviors deliberately apply their effect but
    // return false so the source object survives (the ejectable hijacker is
    // the stock example).  Keep that semantic separate from effectApplied.
    bool preserveSourceOnSuccess = false;
    bool effectApplied = false;
    bool executeBehaviorReturnedTrue = false;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

class ObjectCrateCollideSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    // Crates and candidate pickers are gathered and sorted by ObjectId. The
    // narrow phase reads current ECS transforms directly, so a locomotor move
    // in this same fixed frame is visible without rebuilding the shared world
    // spatial index midway through the frame.
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const ObjectSpatialIndex& spatialIndex,
                const game::terrain::TerrainLogic& terrain,
                const PlayerRegistry& players,
                const GameContentSnapshot& content,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectCratePickupCommand>& outCommands) const;
};

} // namespace engine

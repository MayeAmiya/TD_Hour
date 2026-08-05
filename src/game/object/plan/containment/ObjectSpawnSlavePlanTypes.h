#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace game {

namespace terrain { class TerrainLogic; }

struct ThingTemplate;
enum class LocomotorSetSlot : uint8_t;

struct ObjectSpawnRule final {
    container::Vector<container::String> templateNames;
    uint32_t spawnNumber = 0;
    uint32_t replacementDelayMilliseconds = 0;
    uint32_t initialBurst = 0;
    uint64_t propagateDamageTypesMask = 0;
    bool oneShot = false;
    bool canReclaimOrphans = false;
    bool aggregateHealth = false;
    bool exitByBudding = false;
    bool spawnedRequireSpawner = false;
    bool slavesHaveFreeWill = false;
    uint32_t authoredOrder = 0;
};

struct ObjectMobNexusRule final {
    uint32_t slots = 1;
    math::q32_32 exitPitchRate{};
    math::q32_32 healthRegenPerSecond{};
    container::String exitBone;
    container::String initialPayloadTemplate;
    uint32_t initialPayloadCount = 0;
    bool scatterNearbyOnExit = false;
    bool orientLikeContainerOnExit = false;
    bool keepContainerVelocityOnExit = false;
    uint32_t authoredOrder = 0;
};

// Load-time HordeUpdate Action. Frame paths branch on this enum only.
enum class ObjectHordeActionKind : uint8_t {
    Horde = 0,
    HordeFixed,
};

struct ObjectHordeRule final {
    // RefCode defaults to one legacy logic second (30 frames).
    uint32_t updateRateMilliseconds = 1000;
    ObjectKindOfMask kindOf{};
    container::Vector<container::String> flagSubObjectNames;
    // Diagnostic only; runtime uses actionKind.
    container::String action;
    ObjectHordeActionKind actionKind = ObjectHordeActionKind::HordeFixed;
    uint32_t count = 0;
    math::q32_32 radius{};
    math::q32_32 rubOffRadius{20};
    bool alliesOnly = true;
    bool exactMatch = false;
    bool allowedNationalism = true;
    uint32_t authoredOrder = 0;
};

struct ObjectTensileFormationRule final {
    container::String crackSound;
    bool enabled = false;
    uint32_t authoredOrder = 0;
};

struct ObjectSlavedRule final {
    uint32_t guardMaxRange = 0;
    uint32_t guardWanderRange = 0;
    uint32_t attackRange = 0;
    uint32_t attackWanderRange = 0;
    uint32_t scoutRange = 0;
    uint32_t scoutWanderRange = 0;
    uint32_t repairRange = 0;
    uint32_t distanceToTargetForRangeBonus = 0;
    uint32_t repairWhenBelowHealthPercent = 0;
    math::q32_32 repairMinAltitude{};
    math::q32_32 repairMaxAltitude{};
    math::q32_32 repairRatePerSecond{};
    uint32_t repairMinReadyMilliseconds = 0;
    uint32_t repairMaxReadyMilliseconds = 0;
    uint32_t repairMinWeldMilliseconds = 0;
    uint32_t repairMaxWeldMilliseconds = 0;
    container::String repairWeldingSystem;
    container::String repairWeldingFxBone;
    bool stayOnSameLayerAsMaster = false;
    uint32_t authoredOrder = 0;
};

struct ObjectMobMemberSlavedRule final {
    // RefCode constructor defaults. Shipped AngryMob content overrides these
    // with 40/15, but map/mod objects are allowed to omit both fields.
    uint32_t mustCatchUpRadius = 50;
    uint32_t noNeedToCatchUpRadius = 25;
    // Legacy field name says Time; shipped comments clarify that this is a
    // count of sparse MobMemberSlavedUpdate calls, not logic frames/ms.
    uint32_t catchUpCrisisBailFrames = 999999;
    math::q32_32 squirrelliness{};
    uint32_t authoredOrder = 0;
};

struct ObjectSpawnSlavePlan final {
    container::Vector<ObjectSpawnRule> spawns;
    container::Vector<ObjectMobNexusRule> mobNexus;
    container::Vector<ObjectHordeRule> hordes;
    container::Vector<ObjectTensileFormationRule> tensileFormations;
    container::Vector<ObjectSlavedRule> slaved;
    container::Vector<ObjectMobMemberSlavedRule> mobMemberSlaved;
    bool hiveStructureBody = false;
};

[[nodiscard]] container::SharedPtr<const ObjectSpawnSlavePlan>
compileObjectSpawnSlavePlan(const ThingTemplate& templateData);

} // namespace game

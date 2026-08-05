#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/structure/ObjectMinefieldPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;
class SimulationRandom;
struct ObjectDamageRequest;
struct ObjectSimulationRules;
struct ObjectSystemWeaponFireCommand;

struct ObjectGeneratedMineRecord final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t generatorIndex = 0;
};

struct ObjectGenerateMinefieldRuntime final {
    LogicFixedVec3 target{};
    uint64_t lastUpdateTick = UINT64_MAX;
    bool hasTarget = false;
    bool generated = false;
    bool upgraded = false;
};

struct ObjectMineImmunity final {
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t lastContactTick = 0;
};

struct ObjectMineDetonator final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 lastPosition{};
};

struct ObjectMinefieldRuntime final {
    game::WeaponContentId detonationWeapon;
    game::ObjectCreationListContentId creationList;
    container::Vector<ObjectMineImmunity> immunities;
    container::Vector<ObjectMineDetonator> detonators;
    LogicFixedVec3 scootTarget{};
    LogicFixedVec3 scootVelocity{};
    LogicFixedVec3 scootAcceleration{};
    math::q32_32 previousHealth{};
    uint64_t nextCreatorDeathCheckTick = 0;
    uint64_t scootEndTick = 0;
    uint64_t lastUpdateTick = UINT64_MAX;
    uint32_t virtualMinesRemaining = 0;
    uint32_t nextShotSequence = 1;
    bool regenerates = false;
    bool draining = false;
    bool scooting = false;
    bool ownsHealthFloor = false;
};

struct ObjectDemoTrapRuntime final {
    game::WeaponContentId detonationWeapon;
    uint64_t nextScanTick = 0;
    uint64_t lastUpdateTick = UINT64_MAX;
    uint32_t nextShotSequence = 1;
    bool detonated = false;
    bool ownsModeLock = false;
};

struct ObjectMinefieldComponent final {
    container::SharedPtr<const game::ObjectMinefieldPlan> plan;
    container::Vector<ObjectGenerateMinefieldRuntime> generators;
    container::Vector<ObjectMinefieldRuntime> mines;
    container::Vector<ObjectDemoTrapRuntime> demoTraps;
};

struct ObjectMineSpawnCommand final {
    container::String templateName;
    ObjectId producer = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    LogicFixedVec3 position{};
    math::q32_32 yaw{};
    LogicFixedVec3 scootStart{};
    uint32_t generatorIndex = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectMinefieldFxEvent final {
    ObjectId source = INVALID_OBJECT_ID;
    container::String fxList;
    LogicFixedVec3 position{};
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

class ObjectMinefieldSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const GameContentSnapshot& content,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSpatialIndex* spatialIndex,
                SimulationRandom& random,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick, uint64_t& nextEmissionSequence,
                container::Vector<ObjectDamageRequest>& outDamage,
                container::Vector<ObjectSystemWeaponFireCommand>& outWeapons,
                container::Vector<ObjectCreationListInvocation>& outOcl,
                container::Vector<ObjectMineSpawnCommand>& outSpawns,
                container::Vector<ObjectMinefieldFxEvent>& outFx) const;

    [[nodiscard]] bool setGeneratorTarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3* target) const;
    [[nodiscard]] bool configureMineScoot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId mine, const LogicFixedVec3& start,
        const LogicFixedVec3& target,
        const ObjectSimulationRules& rules, uint64_t confirmedTick) const;
    [[nodiscard]] bool setDemoTrapMode(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId trap, bool proximityMode) const;
    [[nodiscard]] bool triggerDemoTrap(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId trap) const;
    [[nodiscard]] bool disarmMine(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId mine, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage) const;
    [[nodiscard]] bool onMinefieldDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId mine, uint32_t authoredOrder,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool onGenerateMinefieldDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const GameContentSnapshot& content,
        const game::terrain::TerrainLogic& terrain,
        SimulationRandom& random, const ObjectSimulationRules& rules,
        ObjectId generator, uint32_t authoredOrder,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectMineSpawnCommand>& outSpawns,
        container::Vector<ObjectMinefieldFxEvent>& outFx) const;
};

} // namespace engine

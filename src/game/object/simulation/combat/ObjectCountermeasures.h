#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/combat/ObjectCountermeasuresPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class SimulationRandom;
class UpgradeCatalog;

enum class ObjectCountermeasurePhase : uint8_t {
    Inactive,
    Idle,
    AwaitReaction,
    FiringSequence,
    Reloading,
    EmptyManual,
};

struct ObjectCountermeasureRuleRuntime final {
    container::Vector<ObjectId> flares;
    uint64_t reactionDueTick = 0;
    uint64_t nextVolleyDueTick = 0;
    uint64_t reloadDueTick = 0;
    uint64_t incomingMissiles = 0;
    uint64_t divertedMissiles = 0;
    uint32_t availableFlares = 0;
    uint32_t pendingFlareSpawns = 0;
    uint32_t launchSequence = 0;
    uint32_t delayBetweenVolleysTicks = 0;
    uint32_t reloadTicks = 0;
    uint32_t missileDecoyTicks = 0;
    uint32_t reactionLatencyTicks = 0;
    ObjectCountermeasurePhase phase = ObjectCountermeasurePhase::Inactive;
    bool upgradeActive = false;
};

struct ObjectCountermeasuresComponent final {
    container::SharedPtr<const game::ObjectCountermeasuresPlan> plan;
    container::Vector<ObjectCountermeasureRuleRuntime> rules;
};

struct ObjectCountermeasureFlareSpawnCommand final {
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t ruleIndex = 0;
    uint32_t authoredOrder = 0;
    uint32_t launchSequence = 0;
    uint32_t flareOrdinal = 0;
    container::String flareTemplate;
    LogicFixedVec3 position{};
    math::q32_32 orientationRadians{};
    LogicFixedVec3 inheritedVelocityUnitsPerSecond{};
    LogicFixedVec3 motiveForce{};
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectCountermeasureEventKind : uint8_t {
    ThreatReported,
    DiversionScheduled,
    VolleyLaunched,
    MissileRetargeted,
    Reloaded,
};

struct ObjectCountermeasureEvent final {
    ObjectCountermeasureEventKind kind =
        ObjectCountermeasureEventKind::ThreatReported;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId projectile = INVALID_OBJECT_ID;
    ObjectId flare = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint32_t sequence = 0;
    uint64_t confirmedTick = 0;
};

class ObjectCountermeasuresSystem final {
public:
    void reset() noexcept;
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          uint32_t logicFramesPerSecond) const;
    void reevaluateObject(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object,
        const UpgradeMask& playerCompletedUpgrades,
        uint64_t confirmedTick,
        const UpgradeCatalog* catalog = nullptr) const;

    // Called immediately after GameSession materializes a SMALL_MISSILE.
    // The deterministic roll and diversion deadline are committed before
    // the projectile advances, matching Weapon::fireWeaponTemplate.
    [[nodiscard]] bool reportIncomingMissile(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId victim, ObjectId projectile, SimulationRandom& random,
        uint64_t confirmedTick);

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                uint64_t confirmedTick);
    void acknowledgeFlareSpawn(ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ObjectId source, uint32_t ruleIndex,
                               ObjectId flare, bool created,
                               uint64_t confirmedTick);
    void resolveMissileDiversions(ecs::registry& registry,
                                  const ObjectLifecycle& lifecycle,
                                  uint64_t confirmedTick);
    [[nodiscard]] bool reload(ecs::registry& registry,
                              const ObjectLifecycle& lifecycle,
                              ObjectId object,
                              uint64_t confirmedTick);

    [[nodiscard]] container::Vector<ObjectCountermeasureFlareSpawnCommand>
    takeFlareSpawnCommands();
    void drainFlareSpawnCommands(
        container::Vector<ObjectCountermeasureFlareSpawnCommand>& out);
    void discardFlareSpawnCommands() noexcept;
    [[nodiscard]] container::Vector<ObjectCountermeasureEvent> takeEvents();

private:
    container::Vector<ObjectCountermeasureFlareSpawnCommand> m_spawnCommands;
    container::Vector<ObjectCountermeasureEvent> m_events;
};

} // namespace engine

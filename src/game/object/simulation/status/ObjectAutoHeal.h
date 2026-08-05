#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"

#include <cstdint>
#include <limits>
#include "core/ecs/registry.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include "game/object/plan/status/ObjectAutoHealPlanTypes.h"
namespace engine
{

struct ObjectSimulationRules;
struct ObjectDamageRequest;
class ObjectLifecycle;
class ObjectOwnershipIndex;
class PlayerRegistry;
class SimulationRandom;
class UpgradeCatalog;

// One runtime entry exists per authored AutoHealBehavior because stock
// recipes may legitimately carry multiple heal modules (for example the
// ambulance's self and area heal declarations).  All timers are confirmed
// frame numbers; no renderer delta or wall clock participates in healing.
struct ObjectAutoHealRuntime final
{
    static constexpr uint64_t NeverWakeTick = std::numeric_limits<uint64_t>::max();

    uint64_t nextWakeTick = NeverWakeTick;
    uint64_t soonestHealTick = 0;
    uint64_t lastUpdateTick = NeverWakeTick;
    // UpgradeMux::m_upgradeExecuted is distinct from this behavior's waking
    // state. `RemovesUpgrades` clears only the former; it never rewinds a
    // prior healing implementation or SingleBurst side effect.
    bool upgradeActivated = false;
    bool active = false;
    bool stopped = false;
    bool radiusEmitterActive = false;
};

struct ObjectAutoHealComponent final
{
    container::SharedPtr<const game::ObjectAutoHealPlan> plan;
    container::Vector<ObjectAutoHealRuntime> instances;
};

// Shared non-stacking lease used by radius AutoHeal and PropagandaTower.
// The original Object fields allow only one sole healing source at a time.
struct ObjectSoleHealingBenefactorComponent final
{
    ObjectId source = INVALID_OBJECT_ID;
    uint64_t expiresTick = 0;
};

enum class ObjectAutoHealParticleEventKind : uint8_t
{
    RadiusBegin,
    RadiusEnd,
    UnitPulse,
};

struct ObjectAutoHealParticleEvent final
{
    ObjectAutoHealParticleEventKind kind =
        ObjectAutoHealParticleEventKind::UnitPulse;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    container::String particleSystem;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

// Stateless system facade.  Persistent behavior belongs to
// ObjectAutoHealComponent so session reset/spawn/destroy are ordinary ECS
// lifecycle operations rather than hidden global module ownership.
class ObjectAutoHealSystem final
{
public:
    // Legacy MinefieldBehavior resolves the first authored
    // AutoHealBehavior and calls stopHealing() when its creator dies.  Keep
    // that module-local operation explicit: removing the aggregate ECS
    // component would incorrectly stop every authored AutoHealBehavior on
    // the object.
    [[nodiscard]] static bool stopFirstAuthored(
        ecs::registry& registry, ecs::entity entity) noexcept;

    void initializeObject(ecs::registry& registry,
                          ecs::entity entity,
                          const UpgradeMask& ownerCompletedUpgrades,
                          SimulationRandom& random,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    // This is called only from a real PlayerUpgradeCompleted transition, not
    // by polling PlayerStateRevisions each frame.  OwnershipIndex supplies a
    // stable ObjectId order; every id is revalidated against ECS/lifecycle
    // before it is allowed to activate a module.
    void onPlayerUpgradeCompleted(ecs::registry& registry,
                                  ObjectLifecycle& lifecycle,
                                  const ObjectOwnershipIndex& ownership,
                                  PlayerId player,
                                  const UpgradeMask& completedUpgrades,
                                  uint64_t confirmedTick,
                                  const UpgradeCatalog* catalog = nullptr) const;

    // Ownership transfer has the same UpgradeMux re-evaluation point as the
    // source Object::setTeam path. Existing activations remain sticky; this
    // only wakes a still-dormant rule for the new controller's tech set.
    void onObjectOwnerChanged(ecs::registry& registry,
                              ObjectLifecycle& lifecycle,
                              ObjectId object,
                              const UpgradeMask& completedUpgrades,
                              uint64_t confirmedTick,
                              const UpgradeCatalog* catalog = nullptr) const;

    // Called at the health transaction boundary only after a true fixed-point
    // HP decrease.  A rejected/zero damage request and a healing event never
    // reach this method.
    void onHealthDecreased(ecs::registry& registry,
                           ObjectLifecycle& lifecycle,
                           ObjectId object,
                           uint64_t confirmedTick,
                           const ObjectSimulationRules& rules) const;

    // Emits fixed-point HEALING requests in ObjectId then authored-module
    // order. ObjectSimulation remains the single Body/Armor writer and
    // resolves the resulting transactions in the same confirmed frame.
    void update(ecs::registry& registry,
                ObjectLifecycle& lifecycle,
                const PlayerRegistry* players,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage,
                container::Vector<ObjectAutoHealParticleEvent>& outParticles) const;

    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectAutoHealParticleEvent>& outParticles) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
    [[nodiscard]] static uint64_t initialPhaseDelay(uint64_t delayTicks, SimulationRandom& random) noexcept;
    static void activateEligible(ObjectAutoHealComponent& component,
                                 const UpgradeMask& completedUpgrades,
                                 uint64_t confirmedTick,
                                 const UpgradeCatalog* catalog) noexcept;
};

} // namespace engine

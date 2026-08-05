#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/spatial/ObjectSpatialIndex.h"

#include <cstdint>
#include "game/object/plan/combat/ObjectFireWeaponCollidePlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;

struct ObjectFireWeaponCollideRuleRuntime final {
    game::WeaponContentId content;
    uint32_t nextShotSequence = 1;
    bool everFired = false;
};

struct ObjectFireWeaponCollideComponent final {
    container::SharedPtr<const game::ObjectFireWeaponCollidePlan> plan;
    container::Vector<ObjectFireWeaponCollideRuleRuntime> instances;
};

class ObjectFireWeaponCollideSystem final {
public:
    void reset() noexcept {
        container::Vector<ObjectId>{}.swap(m_nearbyScratch);
    }
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content) const;

    // Stage-1 object-contact producer. It reuses the same conservative
    // cylinder/sphere overlap contract as CrateCollide until the full rigid
    // body contact manifold replaces this broad phase.
    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectSpatialIndex& spatialIndex,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectSystemWeaponFireCommand>& outCommands);

private:
    // Presentation-independent, non-authoritative capacity cache. update()
    // owns it exclusively and consumes each query before the next clear.
    container::Vector<ObjectId> m_nearbyScratch;
};

} // namespace engine

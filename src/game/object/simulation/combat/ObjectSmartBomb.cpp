#include "game/object/simulation/combat/ObjectSmartBomb.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine {

void ObjectSmartBombSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!objectTemplate || !objectTemplate->archetype ||
        !objectTemplate->archetype->smartBombPlan) return;
    ObjectSmartBombComponent component;
    component.plan = objectTemplate->archetype->smartBombPlan;
    component.rules.resize(component.plan->rules.size());
    if (ObjectSmartBombComponent* existing =
            ecs::try_get<ObjectSmartBombComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectSmartBombComponent>(registry, entity,
                                               std::move(component));
    }
}

bool ObjectSmartBombSystem::setTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& target) const {
    // (0,0,0) is a valid legacy map coordinate; targetReceived is the
    // explicit optional-state bit and must not overload a sentinel vector.
    if (!object) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    ObjectSmartBombComponent* component = entity
        ? ecs::try_get<ObjectSmartBombComponent>(registry, *entity) : nullptr;
    if (!component) return false;
    for (ObjectSmartBombRuleRuntime& runtime : component->rules) {
        runtime.target = target;
        runtime.targetReceived = true;
    }
    return !component->rules.empty();
}

void ObjectSmartBombSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    struct Candidate { ObjectId id; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectSmartBombComponent,
                                TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        if (identity.id && lifecycle.entityFromId(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.id < right.id;
        });
    const math::q32_32 rate{static_cast<int32_t>(
        std::max<uint32_t>(1, rules.logicFramesPerSecond))};
    const math::q32_32 significantHeight = math::q32_32::max(
        math::q32_32{}, -math::q32_32{int32_t{9}} *
            rules.gravityUnitsPerSecondSq / (rate * rate));
    for (const Candidate& candidate : candidates) {
        ObjectSmartBombComponent& component =
            ecs::get<ObjectSmartBombComponent>(registry, candidate.entity);
        TransformComponent& transform =
            ecs::get<TransformComponent>(registry, candidate.entity);
        LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, candidate.entity, transform);
        const math::q32_32 ground = terrain.isLoaded()
            ? math::q32_32::from_raw(
                  terrain.groundHeightRaw(position.x.raw(), position.y.raw()))
            : math::q32_32{};
        const math::q32_32 currentZ = position.z;
        if (currentZ - ground <= significantHeight || !component.plan) continue;
        const size_t count = std::min(component.rules.size(),
                                      component.plan->rules.size());
        bool changed = false;
        for (size_t index = 0; index < count; ++index) {
            const ObjectSmartBombRuleRuntime& runtime = component.rules[index];
            if (!runtime.targetReceived) continue;
            const math::q32_32 self =
                component.plan->rules[index].courseCorrectionScalar;
            const math::q32_32 target = math::q32_32{int32_t{1}} - self;
            position.x = runtime.target.x * target + position.x * self;
            position.y = runtime.target.y * target + position.y * self;
            changed = true;
        }
        if (!changed) continue;
        writeAuthoritativeObjectPosition(
            registry, candidate.entity, position);
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                     candidate.entity)) {
            physics->position.x = position.x;
            physics->position.y = position.y;
            physics->lastPublishedPosition.x = position.x;
            physics->lastPublishedPosition.y = position.y;
        }
        static_cast<void>(confirmedTick);
    }
}
} // namespace engine

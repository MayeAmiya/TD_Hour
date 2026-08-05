#include "core/container/container_types.h"
#include "game/object/simulation/movement/ObjectFloat.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <utility>

namespace engine {
namespace {

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

} // namespace

void ObjectFloatSystem::initializeObject(ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->floatPlan ||
        templateComponent->archetype->floatPlan->rules.empty()) {
        return;
    }

    ObjectFloatComponent component{
        .plan = templateComponent->archetype->floatPlan,
    };
    component.instances.reserve(component.plan->rules.size());
    for (const game::ObjectFloatRule& rule : component.plan->rules) {
        component.instances.push_back({.enabled = rule.startsEnabled});
    }
    if (ObjectFloatComponent* existing = ecs::try_get<ObjectFloatComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectFloatComponent>(registry, entity, std::move(component));
    }
}

bool ObjectFloatSystem::setEnabled(ecs::registry& registry,
                                   const ObjectLifecycle& lifecycle,
                                   const ObjectFloatEnableRequest& request) const {
    if (!request.object || lifecycle.isPendingDestroy(request.object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(request.object);
    if (!entity) return false;
    ObjectFloatComponent* component = ecs::try_get<ObjectFloatComponent>(registry, *entity);
    if (!component || !component->plan) return false;

    const size_t count = std::min(component->plan->rules.size(), component->instances.size());
    bool matched = false;
    for (size_t index = 0; index < count; ++index) {
        if (request.authoredOrder &&
            component->plan->rules[index].authoredOrder != *request.authoredOrder) {
            continue;
        }
        component->instances[index].enabled = request.enabled;
        matched = true;
    }
    return matched;
}

void ObjectFloatSystem::update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                               const game::terrain::TerrainLogic& terrain,
                               uint64_t confirmedTick) const {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectFloatComponent,
                                const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    for (const Candidate& candidate : candidates) {
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }
        ObjectFloatComponent& component = ecs::get<ObjectFloatComponent>(registry, candidate.entity);
        component.visualSampleTick = confirmedTick;
        const TransformComponent& transform = ecs::get<const TransformComponent>(registry, candidate.entity);
        if (!component.plan) continue;

        const size_t count = std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            if (!component.instances[index].enabled) continue;

            LogicFixedVec3 position =
                readAuthoritativeObjectPosition(registry, candidate.entity, transform);
            const std::optional<int64_t> waterHeight =
                terrain.waterSurfaceHeightLegacyRawAt(
                    position.x.raw(), position.y.raw());
            // The legacy no-water path used an uninitialized output value.
            // A modern ECS simulation makes that undefined behavior an
            // explicit, deterministic no-op while preserving every valid
            // water-surface snap.
            if (!waterHeight) continue;

            position.z = math::q32_32::from_raw(*waterHeight);
            writeAuthoritativeObjectPosition(registry, candidate.entity, position);
        }
    }
}

} // namespace engine

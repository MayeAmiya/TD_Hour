#include "game/object/simulation/world/ObjectRadiusDecal.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] std::optional<ecs::entity> liveEntity(
    const ObjectLifecycle& lifecycle, ObjectId object) {
    if (!object || lifecycle.isPendingDestroy(object)) return std::nullopt;
    return lifecycle.entityFromId(object);
}

[[nodiscard]] PlayerId ownerOf(const ecs::registry& registry,
                               ecs::entity entity) noexcept {
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    return owner ? owner->player : INVALID_PLAYER_ID;
}

// RefCode's OBJECT_STATUS_IS_ATTACKING is raised by AIAttackState::onEnter and
// cleared by onExit, i.e. it is true for exactly as long as the object owns an
// attack it is executing.  This runtime has no single writer of that status
// bit, so recognize the same fact from the two authoritative pieces of state:
// an Attack intent at the queue head, or a non-idle weapon runtime.  Without
// this, a killWhenNoLongerAttacking decal published by OCL's Attack nugget is
// destroyed by the very same confirmed frame that created it, before Combat
// has had a chance to observe the freshly queued order.
[[nodiscard]] bool isExecutingAttack(const ecs::registry& registry,
                                     ecs::entity entity) noexcept {
    const ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    if (queue && !queue->orders.empty() &&
        queue->orders.front().kind == ObjectOrderKind::Attack) {
        return true;
    }
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    return weapons &&
        weapons->state != ObjectWeaponRuntimeState::Idle &&
        weapons->state != ObjectWeaponRuntimeState::NoUsableWeapon;
}

void emitEndIfActive(
    const ecs::registry& registry, ecs::entity entity, ObjectId object,
    const game::ObjectRadiusDecalRule& rule,
    ObjectRadiusDecalRuntime& runtime, uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) {
    if (!runtime.active) return;
    outEvents.push_back({
        .kind = ObjectRadiusDecalEventKind::End,
        .object = object,
        .owner = ownerOf(registry, entity),
        .authoredOrder = rule.authoredOrder,
        .texture = runtime.texture,
        .position = runtime.position,
        .radius = runtime.radius,
        .shadowTypeMask = runtime.shadowTypeMask,
        .minimumOpacity = runtime.minimumOpacity,
        .maximumOpacity = runtime.maximumOpacity,
        .opacityThrobTicks = runtime.opacityThrobTicks,
        .color = runtime.color,
        .usesPlayerColor = runtime.usesPlayerColor,
        .onlyVisibleToOwningPlayer = runtime.onlyVisibleToOwningPlayer,
        .confirmedTick = confirmedTick,
    });
    runtime = {};
}

} // namespace

void ObjectRadiusDecalSystem::initializeObject(ecs::registry& registry,
                                               ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectRadiusDecalPlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->radiusDecalPlan
            : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectRadiusDecalComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    if (ObjectRadiusDecalComponent* existing =
            ecs::try_get<ObjectRadiusDecalComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectRadiusDecalComponent>(registry, entity,
                                                 std::move(component));
    }
}

bool ObjectRadiusDecalSystem::createRadiusDecal(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRadiusDecalRequest& request,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const {
    if (request.texture.empty() || request.radius <= math::q32_32{}) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        liveEntity(lifecycle, request.object);
    if (!entity) return false;
    ObjectRadiusDecalComponent* component =
        ecs::try_get<ObjectRadiusDecalComponent>(registry, *entity);
    if (!component || !component->plan || component->plan->rules.empty() ||
        component->instances.size() != component->plan->rules.size()) {
        return false;
    }

    ObjectRadiusDecalRuntime& runtime = component->instances.front();
    const game::ObjectRadiusDecalRule& rule = component->plan->rules.front();
    emitEndIfActive(registry, *entity, request.object, rule, runtime,
                    request.confirmedTick, outEvents);
    runtime.active = true;
    runtime.killWhenNoLongerAttacking = request.killWhenNoLongerAttacking;
    runtime.texture = request.texture;
    runtime.position = request.position;
    runtime.radius = request.radius;
    runtime.shadowTypeMask = request.shadowTypeMask;
    runtime.minimumOpacity = request.minimumOpacity;
    runtime.maximumOpacity = request.maximumOpacity;
    runtime.opacityThrobTicks = request.opacityThrobTicks;
    runtime.color = request.color;
    runtime.usesPlayerColor = request.usesPlayerColor;
    runtime.onlyVisibleToOwningPlayer = request.onlyVisibleToOwningPlayer;
    outEvents.push_back({
        .kind = ObjectRadiusDecalEventKind::Begin,
        .object = request.object,
        .owner = ownerOf(registry, *entity),
        .authoredOrder = rule.authoredOrder,
        .texture = runtime.texture,
        .position = runtime.position,
        .radius = runtime.radius,
        .shadowTypeMask = runtime.shadowTypeMask,
        .minimumOpacity = runtime.minimumOpacity,
        .maximumOpacity = runtime.maximumOpacity,
        .opacityThrobTicks = runtime.opacityThrobTicks,
        .color = runtime.color,
        .usesPlayerColor = runtime.usesPlayerColor,
        .onlyVisibleToOwningPlayer = runtime.onlyVisibleToOwningPlayer,
        .confirmedTick = request.confirmedTick,
    });
    return true;
}

bool ObjectRadiusDecalSystem::killRadiusDecal(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId object,
    uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const {
    if (!object) return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectRadiusDecalComponent* component =
        ecs::try_get<ObjectRadiusDecalComponent>(registry, *entity);
    if (!component || !component->plan ||
        component->instances.size() != component->plan->rules.size()) {
        return false;
    }
    bool killed = false;
    for (size_t index = 0; index < component->instances.size(); ++index) {
        const bool wasActive = component->instances[index].active;
        emitEndIfActive(registry, *entity, object, component->plan->rules[index],
                        component->instances[index], confirmedTick, outEvents);
        killed = killed || wasActive;
    }
    return killed;
}

void ObjectRadiusDecalSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectRadiusDecalComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    constexpr game::ObjectStatusMask kAttacking =
        game::objectStatusBit(game::ObjectStatusFlag::IsAttacking);
    for (const Candidate& candidate : candidates) {
        ObjectRadiusDecalComponent& component =
            ecs::get<ObjectRadiusDecalComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        if (status && status->hasAny(kAttacking)) continue;
        if (isExecutingAttack(registry, candidate.entity)) continue;
        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectRadiusDecalRuntime& runtime = component.instances[index];
            if (!runtime.active || !runtime.killWhenNoLongerAttacking) continue;
            emitEndIfActive(registry, candidate.entity, candidate.object,
                            component.plan->rules[index], runtime,
                            confirmedTick, outEvents);
        }
    }
}

} // namespace engine

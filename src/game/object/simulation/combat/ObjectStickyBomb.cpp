#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "core/container/string_utils.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace engine {
namespace {

using Fixed = math::q32_32;

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    return left > std::numeric_limits<uint64_t>::max() - right
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] bool effectivelyDead(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ecs::entity entity) noexcept {
    if (!object || lifecycle.isPendingDestroy(object)) return true;
    const ObjectLifecycleComponent* lifecycleState =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    if (lifecycleState &&
        lifecycleState->phase != ObjectLifecyclePhase::Alive) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && health->effectivelyDead;
}

[[nodiscard]] LogicFixedVec3 authoritativePosition(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    return transform
        ? readAuthoritativeObjectPosition(registry, entity, *transform)
        : LogicFixedVec3{};
}

[[nodiscard]] bool sameMapStatus(const ecs::registry& registry,
                                 ecs::entity left,
                                 ecs::entity right) noexcept {
    const ObjectMapStatusComponent* a =
        ecs::try_get<ObjectMapStatusComponent>(registry, left);
    const ObjectMapStatusComponent* b =
        ecs::try_get<ObjectMapStatusComponent>(registry, right);
    return (a && a->offMap) == (b && b->offMap);
}

} // namespace

void ObjectStickyBombSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype
        ? type->archetype->stickyBombPlan : nullptr;
    if (!plan || plan->rules.empty()) return;
    ObjectStickyBombComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    component.boobyTrap = hasKind(
        ecs::try_get<ObjectKindOfComponent>(registry, entity),
        game::ObjectKindOf::BoobyTrap);
    if (ObjectStickyBombComponent* existing =
            ecs::try_get<ObjectStickyBombComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectStickyBombComponent>(
            registry, entity, std::move(component));
    }
}

bool ObjectStickyBombSystem::attach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectStickyBombAttachRequest& request,
    std::optional<uint64_t> lifetimeDueTick,
    uint32_t logicFramesPerSecond,
    container::Vector<ObjectStickyBombPresentationEvent>& events) const {
    if (!request.bomb || !request.target) return false;
    const std::optional<ecs::entity> bombEntity =
        lifecycle.entityFromId(request.bomb);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(request.target);
    if (!bombEntity || !targetEntity ||
        effectivelyDead(registry, lifecycle, request.target, *targetEntity)) {
        return false;
    }
    ObjectStickyBombComponent* component =
        ecs::try_get<ObjectStickyBombComponent>(registry, *bombEntity);
    if (!component || !component->plan || component->instances.empty()) {
        return false;
    }
    if (component->boobyTrap) {
        const ObjectStatusComponent* targetStatus =
            ecs::try_get<ObjectStatusComponent>(registry, *targetEntity);
        if (targetStatus && targetStatus->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::BoobyTrapped))) {
            return false;
        }
    }

    LogicFixedVec3 position = authoritativePosition(
        registry, *targetEntity);
    const bool immobile = hasKind(
        ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity),
        game::ObjectKindOf::Immobile);
    bool fixedToGround = false;
    if (request.specificPosition) {
        position = *request.specificPosition;
        fixedToGround = true;
    } else if (immobile) {
        if (request.bomber) {
            if (const std::optional<ecs::entity> bomberEntity =
                    lifecycle.entityFromId(request.bomber)) {
                position = authoritativePosition(registry, *bomberEntity);
            }
        }
        fixedToGround = true;
    } else {
        position.z += component->plan->rules.front().offsetZ;
    }
    if (fixedToGround) {
        position.z = terrain.isLoaded()
            ? math::q32_32::from_raw(
                  terrain.groundHeightRaw(position.x.raw(), position.y.raw()))
            : math::q32_32{};
    }
    writeAuthoritativeObjectPosition(registry, *bombEntity, position);

    const uint64_t framesPerSecond =
        std::max<uint32_t>(1u, logicFramesPerSecond);
    uint64_t nextPing = saturatingAdd(
        request.confirmedTick, framesPerSecond);
    if (lifetimeDueTick) {
        const uint64_t remaining = *lifetimeDueTick > request.confirmedTick
            ? *lifetimeDueTick - request.confirmedTick : 0;
        const uint64_t wholeSeconds = remaining / framesPerSecond;
        nextPing = *lifetimeDueTick - wholeSeconds * framesPerSecond;
    }
    const size_t count = std::min(
        component->instances.size(), component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        component->instances[index] = {
            .target = request.target,
            .bomber = request.bomber,
            .fixedGroundPosition = position,
            .dieTick = lifetimeDueTick,
            .nextPingTick = nextPing,
            .attached = true,
            .fixedToGround = fixedToGround,
        };
    }
    if (ObjectProducerComponent* producer =
            ecs::try_get<ObjectProducerComponent>(registry, *bombEntity)) {
        producer->producer = request.target;
    } else {
        ecs::emplace<ObjectProducerComponent>(
            registry, *bombEntity,
            ObjectProducerComponent{.producer = request.target});
    }
    if (component->boobyTrap) {
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *targetEntity,
            {.setMask = game::objectStatusBit(
                 game::ObjectStatusFlag::BoobyTrapped),
             .confirmedTick = request.confirmedTick}));
    }
    if (!component->plan->createdSound.empty()) {
        events.push_back({
            .kind = sticky_bomb::PresentationKind::CreatedAudio,
            .bomb = request.bomb,
            .target = request.target,
            .position = position,
            .resource = component->plan->createdSound,
            .authoredOrder = component->plan->rules.front().authoredOrder,
            .confirmedTick = request.confirmedTick,
        });
    }
    markObjectDirty(
        registry, *bombEntity,
        ObjectDirtyDomain::RenderExtraction);
    return true;
}

bool ObjectStickyBombSystem::retarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId bomb, ObjectId target) const {
    if (!bomb || !target || !lifecycle.entityFromId(target)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(bomb);
    ObjectStickyBombComponent* component = entity
        ? ecs::try_get<ObjectStickyBombComponent>(registry, *entity) : nullptr;
    if (!component) return false;
    bool changed = false;
    for (ObjectStickyBombRuntime& runtime : component->instances) {
        if (!runtime.attached || runtime.detonated) continue;
        runtime.target = target;
        changed = true;
    }
    if (changed) {
        if (ObjectProducerComponent* producer =
                ecs::try_get<ObjectProducerComponent>(registry, *entity)) {
            producer->producer = target;
        }
        markObjectDirty(
            registry, *entity,
            ObjectDirtyDomain::RenderExtraction);
    }
    return changed;
}

bool ObjectStickyBombSystem::detonate(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId bomb,
    sticky_bomb::DetonationTrigger trigger, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damage,
    container::Vector<ObjectStickyBombPresentationEvent>& events) const {
    const std::optional<ecs::entity> bombEntity =
        lifecycle.entityFromId(bomb);
    ObjectStickyBombComponent* component = bombEntity
        ? ecs::try_get<ObjectStickyBombComponent>(registry, *bombEntity)
        : nullptr;
    if (!component || !component->plan || component->instances.empty()) {
        return false;
    }
    ObjectStickyBombRuntime& runtime = component->instances.front();
    if (!runtime.attached || runtime.detonated) return false;
    if (trigger == sticky_bomb::DetonationTrigger::Timed &&
        (!runtime.dieTick || confirmedTick < *runtime.dieTick)) {
        return false;
    }
    const ObjectId target = runtime.target;
    const std::optional<ecs::entity> targetEntity = target
        ? lifecycle.entityFromIdIncludingPending(target) : std::nullopt;
    const game::ObjectStickyBombRule& rule = component->plan->rules.front();
    if (!rule.geometryBasedDamageWeapon.empty() && targetEntity) {
        if (const game::WeaponTemplate* weapon =
                content.findWeapon(rule.geometryBasedDamageWeapon)) {
            const LogicFixedVec3 center = authoritativePosition(
                registry, *targetEntity);
            const ObjectGeometryComponent* hostGeometry =
                ecs::try_get<ObjectGeometryComponent>(registry, *targetEntity);
            const Fixed hostRadius = hostGeometry
                ? Fixed::max(Fixed{},
                    hostGeometry->boundingCircleRadiusFixed)
                : Fixed{};
            const Fixed primaryRadius = hostRadius +
                Fixed::max(Fixed{}, weapon->fixed.primaryDamageRadius);
            const Fixed secondaryRadius = hostRadius +
                Fixed::max(Fixed{}, weapon->fixed.secondaryDamageRadius);
            const Fixed maximumRadius = Fixed::max(
                primaryRadius, secondaryRadius);
            const Fixed maximumRadiusSquared =
                maximumRadius * maximumRadius;
            const Fixed primaryRadiusSquared =
                primaryRadius * primaryRadius;
            const Fixed primaryDamage = weapon->fixed.primaryDamage;
            const Fixed secondaryDamage = weapon->fixed.secondaryDamage;

            struct Candidate final {
                ObjectId id = INVALID_OBJECT_ID;
                ecs::entity entity = ecs::null;
                Fixed distanceSquared{};
            };
            container::Vector<Candidate> candidates;
            const auto view = ecs::view<const ObjectIdentityComponent,
                                        const TransformComponent>(registry);
            candidates.reserve(view.size_hint());
            for (const ecs::entity entity : view) {
                const ObjectIdentityComponent& identity =
                    view.template get<const ObjectIdentityComponent>(entity);
                if (!identity.id || !lifecycle.entityFromId(identity.id) ||
                    lifecycle.isPendingDestroy(identity.id) ||
                    !sameMapStatus(registry, *targetEntity, entity)) {
                    continue;
                }
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry, entity);
                if (health && health->effectivelyDead) continue;
                const LogicFixedVec3 position = authoritativePosition(
                    registry, entity);
                const Fixed dx = position.x - center.x;
                const Fixed dy = position.y - center.y;
                const Fixed dz = position.z - center.z;
                const Fixed distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared > maximumRadiusSquared) continue;
                candidates.push_back({identity.id, entity, distanceSquared});
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& left, const Candidate& right) {
                return left.id < right.id;
            });
            for (const Candidate& candidate : candidates) {
                const Fixed amount = candidate.distanceSquared <=
                        primaryRadiusSquared
                    ? primaryDamage : secondaryDamage;
                damage.push_back({
                    .target = candidate.id,
                    .source = bomb,
                    .sourceSequence = rule.authoredOrder,
                    .causalGroup = bomb,
                    .amount = amount,
                    .damageType = weapon->damageType,
                    .damageStatusMask = weapon->damageStatusMask,
                    .deathType = weapon->deathType,
                    .confirmedTick = confirmedTick,
                });
            }
            if (!rule.geometryBasedDamageFx.empty()) {
                events.push_back({
                    .kind = sticky_bomb::PresentationKind::GeometryDamageFx,
                    .bomb = bomb,
                    .target = target,
                    .position = center,
                    .resource = rule.geometryBasedDamageFx,
                    .overrideRadius = secondaryRadius,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
        }
    }

    if (component->boobyTrap && targetEntity) {
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *targetEntity,
            {.clearMask = game::objectStatusBit(
                 game::ObjectStatusFlag::BoobyTrapped),
             .confirmedTick = confirmedTick}));
    }
    for (ObjectStickyBombRuntime& instance : component->instances) {
        instance.detonated = true;
    }
    markObjectDirty(
        registry, *bombEntity,
        ObjectDirtyDomain::RenderExtraction);
    // Object::kill() always enters Body/Die. The late phase keeps this kill
    // behind every geometry-expanded victim transaction in the same causal
    // group without depending on ObjectId ordering.
    damage.push_back({
        .target = bomb,
        .sourceSequence = rule.authoredOrder,
        .causalGroup = bomb,
        .damageType = game::DamageType::UNRESISTABLE,
        .deathType = game::DeathType::NORMAL,
        .resolutionPhase = ObjectDamageResolutionPhase::PostDetonationSelfKill,
        .forceKill = true,
        .confirmedTick = confirmedTick,
    });
    return true;
}

bool ObjectStickyBombSystem::detonateHostileBoobyTrapOnTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    ObjectId source, ObjectId target, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damage,
    container::Vector<ObjectStickyBombPresentationEvent>& events) const {
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromId(source);
    const OwnerComponent* sourceOwner = sourceEntity
        ? ecs::try_get<OwnerComponent>(registry, *sourceEntity) : nullptr;
    if (!sourceOwner || !target) return false;

    container::Vector<ObjectId> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const ObjectStickyBombComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        const ObjectStickyBombComponent& component =
            view.template get<const ObjectStickyBombComponent>(entity);
        if (!identity.id || !component.boobyTrap ||
            players.relationship(sourceOwner->player, owner.player) !=
                PlayerRelationship::Enemies) {
            continue;
        }
        const bool attached = std::any_of(
            component.instances.begin(), component.instances.end(),
            [target](const ObjectStickyBombRuntime& runtime) {
                return runtime.attached && !runtime.detonated &&
                    runtime.target == target;
            });
        if (attached) candidates.push_back(identity.id);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const ObjectId bomb : candidates) {
        // Object::checkAndDetonateBoobyTrap selects one matching attached
        // trap and returns immediately.  Multiple valid traps are malformed
        // Mod state; keep a deterministic ObjectId fallback without turning
        // one death edge into an authored multi-detonation.
        if (detonate(
                registry, lifecycle, content, bomb,
                sticky_bomb::DetonationTrigger::BoobyTrap, confirmedTick,
                damage, events)) {
            return true;
        }
    }
    return false;
}

bool ObjectStickyBombSystem::detonateBoobyTrapsOnDyingTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId target,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damage,
    container::Vector<ObjectStickyBombPresentationEvent>& events) const {
    if (!target) return false;
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromIdIncludingPending(target);
    const ObjectStatusComponent* targetStatus = targetEntity
        ? ecs::try_get<ObjectStatusComponent>(registry, *targetEntity)
        : nullptr;
    if (!targetStatus || !targetStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::BoobyTrapped))) {
        return false;
    }

    container::Vector<ObjectId> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectStickyBombComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectStickyBombComponent& component =
            view.template get<const ObjectStickyBombComponent>(entity);
        if (!identity.id || !component.boobyTrap) continue;
        const bool attached = std::any_of(
            component.instances.begin(), component.instances.end(),
            [target](const ObjectStickyBombRuntime& runtime) {
                return runtime.attached && !runtime.detonated &&
                    runtime.target == target;
            });
        if (attached) candidates.push_back(identity.id);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const ObjectId bomb : candidates) {
        if (detonate(
                registry, lifecycle, content, bomb,
                sticky_bomb::DetonationTrigger::BoobyTrap, confirmedTick,
                damage, events)) {
            return true;
        }
    }
    return false;
}

void ObjectStickyBombSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectStickyBombPresentationEvent>& events) const {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectStickyBombComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId id =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (id && lifecycle.entityFromId(id)) {
            candidates.push_back({id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
        return left.id < right.id;
    });

    const uint64_t framesPerSecond =
        std::max<uint32_t>(1u, rules.logicFramesPerSecond);
    for (const Candidate& candidate : candidates) {
        ObjectStickyBombComponent& component =
            ecs::get<ObjectStickyBombComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const size_t count = std::min(
            component.instances.size(), component.plan->rules.size());
        bool targetDestroyed = false;
        for (size_t index = 0; index < count; ++index) {
            ObjectStickyBombRuntime& runtime = component.instances[index];
            if (!runtime.attached || runtime.detonated) continue;
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(runtime.target);
            if (!targetEntity || effectivelyDead(
                    registry, lifecycle, runtime.target, *targetEntity)) {
                targetDestroyed = true;
                break;
            }
            LogicFixedVec3 position;
            if (runtime.fixedToGround) {
                position = runtime.fixedGroundPosition;
                position.z = terrain.isLoaded()
                    ? math::q32_32::from_raw(
                          terrain.groundHeightRaw(
                              position.x.raw(), position.y.raw()))
                    : math::q32_32{};
                runtime.fixedGroundPosition = position;
            } else {
                position = authoritativePosition(registry, *targetEntity);
                position.z += component.plan->rules[index].offsetZ;
            }
            writeAuthoritativeObjectPosition(
                registry, candidate.entity, position);
            if (confirmedTick >= runtime.nextPingTick) {
                runtime.nextPingTick = saturatingAdd(
                    runtime.nextPingTick, framesPerSecond);
                if (!component.plan->pingSound.empty()) {
                    events.push_back({
                        .kind = sticky_bomb::PresentationKind::PingAudio,
                        .bomb = candidate.id,
                        .target = runtime.target,
                        .position = position,
                        .resource = component.plan->pingSound,
                        .authoredOrder =
                            component.plan->rules[index].authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
        }
        if (targetDestroyed) {
            static_cast<void>(lifecycle.requestDestroy(
                candidate.id, ObjectDestroyReason::System, confirmedTick));
        }
    }
}

std::optional<ObjectStickyBombState> ObjectStickyBombSystem::state(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId bomb) const noexcept {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(bomb);
    const ObjectStickyBombComponent* component = entity
        ? ecs::try_get<ObjectStickyBombComponent>(registry, *entity) : nullptr;
    if (!component || component->instances.empty()) return std::nullopt;
    const ObjectStickyBombRuntime& runtime = component->instances.front();
    return ObjectStickyBombState{
        .bomb = bomb,
        .target = runtime.target,
        .bomber = runtime.bomber,
        .dieTick = runtime.dieTick,
        .nextPingTick = runtime.nextPingTick,
        .timed = runtime.dieTick.has_value(),
        .attached = runtime.attached,
        .fixedToGround = runtime.fixedToGround,
        .detonated = runtime.detonated,
    };
}

} // namespace engine

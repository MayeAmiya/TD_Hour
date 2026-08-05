#include "game/object/simulation/lifecycle/ObjectCleanupHazard.h"

#include "core/container/string_utils.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace engine {
namespace {

using Fixed = math::q32_32;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] Fixed saturatingFixedAdd(Fixed left, Fixed right) noexcept {
    if (right.raw() > 0 &&
        left.raw() > std::numeric_limits<int64_t>::max() - right.raw()) {
        return Fixed::from_raw(std::numeric_limits<int64_t>::max());
    }
    if (right.raw() < 0 &&
        left.raw() < std::numeric_limits<int64_t>::min() - right.raw()) {
        return Fixed::from_raw(std::numeric_limits<int64_t>::min());
    }
    return Fixed::from_raw(left.raw() + right.raw());
}

[[nodiscard]] Fixed distanceSquared2D(const LogicFixedVec3& left,
                                      const LogicFixedVec3& right) noexcept {
    const Fixed dx = left.x - right.x;
    const Fixed dy = left.y - right.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] bool within2D(const LogicFixedVec3& left,
                            const LogicFixedVec3& right,
                            Fixed radius) noexcept {
    if (radius < Fixed{}) return false;
    return distanceSquared2D(left, right) < radius * radius;
}

[[nodiscard]] LogicFixedVec3 pursuitPosition(
    const LogicFixedVec3& source, const LogicFixedVec3& target,
    Fixed weaponRange) noexcept {
    const Fixed dx = target.x - source.x;
    const Fixed dy = target.y - source.y;
    const Fixed distance = Fixed::sqrt(dx * dx + dy * dy);
    constexpr Fixed kRangeMargin{int32_t{5}};
    constexpr Fixed kMinimumStandOff{int32_t{1}};
    const Fixed standOff = Fixed::max(
        kMinimumStandOff, weaponRange > kRangeMargin
            ? weaponRange - kRangeMargin
            : kMinimumStandOff);
    if (distance <= standOff || distance <= Fixed{}) return source;
    const Fixed travelFraction = (distance - standOff) / distance;
    return {
        source.x + dx * travelFraction,
        source.y + dy * travelFraction,
        target.z,
    };
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool sameMapStatus(const ecs::registry& registry,
                                 ecs::entity left,
                                 ecs::entity right) noexcept {
    const ObjectMapStatusComponent* leftStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, left);
    const ObjectMapStatusComponent* rightStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, right);
    return (leftStatus && leftStatus->offMap) ==
           (rightStatus && rightStatus->offMap);
}

[[nodiscard]] bool unavailable(const ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               ecs::entity entity, ObjectId object) noexcept {
    if (!object || !lifecycle.entityFromId(object) ||
        lifecycle.isPendingDestroy(object)) {
        return true;
    }
    const ObjectLifecycleComponent* state =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    if (state && state->phase != ObjectLifecyclePhase::Alive) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && health->effectivelyDead;
}

[[nodiscard]] bool isCleanupOrder(const ObjectOrderIntent& order,
                                  size_t instance) noexcept {
    return order.source == ObjectOrderSource::System &&
           order.systemPurpose == ObjectOrderSystemPurpose::CleanupHazard &&
           order.systemPurposeInstance == instance;
}

void advanceCommandSequence(ObjectCleanupHazardRuntime& runtime) noexcept {
    ++runtime.nextCommandSequence;
    if (runtime.nextCommandSequence == 0) ++runtime.nextCommandSequence;
}

void clearCleanupOrder(ObjectOrderQueueComponent* queue, size_t instance) {
    if (!queue || queue->orders.empty() ||
        !isCleanupOrder(queue->orders.front(), instance)) {
        return;
    }
    queue->orders.erase(queue->orders.begin());
    ++queue->revision;
}

void releaseCleanupLock(ecs::registry& registry, ecs::entity entity,
                        ObjectCleanupHazardRuntime& runtime) {
    if (!runtime.ownsTemporaryWeaponLock) return;
    static_cast<void>(releaseObjectWeaponLock(
        registry, entity, ObjectWeaponLockType::Temporary));
    runtime.ownsTemporaryWeaponLock = false;
}

void replaceCleanupOrder(ObjectOrderQueueComponent& queue,
                         ObjectCleanupHazardRuntime& runtime,
                         size_t instance, PlayerId owner,
                         ObjectOrderKind kind, ObjectId target,
                         const LogicFixedVec3& position,
                         uint64_t confirmedTick) {
    if (!queue.orders.empty() && isCleanupOrder(queue.orders.front(), instance)) {
        const ObjectOrderIntent& current = queue.orders.front();
        const bool sameTarget = kind == ObjectOrderKind::Attack
            ? current.targetObject == target
            : current.hasTargetPosition &&
              current.targetX == position.x &&
              current.targetY == position.y &&
              current.targetZ == position.z;
        if (current.kind == kind && sameTarget) return;
        queue.orders.erase(queue.orders.begin());
    } else if (!queue.orders.empty()) {
        return;
    }

    ObjectOrderIntent order{
        .kind = kind,
        .source = ObjectOrderSource::System,
        .contextPlayer = owner,
        .issuedTick = confirmedTick,
        .sourceSequence = runtime.nextCommandSequence,
        .targetObject = target,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = kind == ObjectOrderKind::Move,
        .systemPurpose = ObjectOrderSystemPurpose::CleanupHazard,
        .systemPurposeInstance = static_cast<uint32_t>(instance),
    };
    queue.orders.insert(queue.orders.begin(), std::move(order));
    ++queue.revision;
    advanceCommandSequence(runtime);
}

[[nodiscard]] std::optional<Fixed> cleanupWeaponRange(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, game::WeaponSlot slot,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick) {
    static_cast<void>(refreshObjectWeaponSet(
        registry, entity, content, logicFramesPerSecond, confirmedTick));
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return std::nullopt;
    }
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= game::kWeaponSlotCount) return std::nullopt;
    const ObjectWeaponSlotRuntime& runtime =
        weapons->sets[*weapons->activeWeaponSetIndex].slots[slotIndex];
    const game::WeaponTemplate* weapon = content.findWeapon(runtime.content);
    if (!weapon || weapon->fixed.attackRange < Fixed{}) {
        return std::nullopt;
    }
    // RefCode deliberately queries this maintenance weapon with a cleared
    // WeaponBonus when validating/tracking CleanupHazardUpdate.
    return weapon->fixed.attackRange;
}

struct TargetCandidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
    LogicFixedVec3 position{};
    Fixed distanceSquared{};
};

[[nodiscard]] std::optional<TargetCandidate> closestHazard(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity sourceEntity, const LogicFixedVec3& center, Fixed radius) {
    if (radius < Fixed{}) return std::nullopt;
    const Fixed radiusSquared = radius * radius;
    std::optional<TargetCandidate> best;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectKindOfComponent,
                                const TransformComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            !sameMapStatus(registry, sourceEntity, entity) ||
            !hasKind(&view.template get<const ObjectKindOfComponent>(entity),
                     game::ObjectKindOf::CleanupHazard)) {
            continue;
        }
        const ObjectLifecycleComponent* state =
            ecs::try_get<ObjectLifecycleComponent>(registry, entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if ((state && state->phase != ObjectLifecyclePhase::Alive) ||
            (health && health->effectivelyDead)) {
            continue;
        }
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, entity,
            view.template get<const TransformComponent>(entity));
        const Fixed squared = distanceSquared2D(center, position);
        if (squared >= radiusSquared) continue;
        if (!best || squared < best->distanceSquared ||
            (squared == best->distanceSquared && identity.id < best->object)) {
            best = TargetCandidate{
                .object = identity.id,
                .entity = entity,
                .position = position,
                .distanceSquared = squared,
            };
        }
    }
    return best;
}

[[nodiscard]] std::optional<TargetCandidate> resolveHazard(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity sourceEntity, ObjectId object) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object) ||
        !sameMapStatus(registry, sourceEntity, *entity) ||
        !hasKind(ecs::try_get<ObjectKindOfComponent>(registry, *entity),
                 game::ObjectKindOf::CleanupHazard)) {
        return std::nullopt;
    }
    const ObjectLifecycleComponent* state =
        ecs::try_get<ObjectLifecycleComponent>(registry, *entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *entity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform || (state && state->phase != ObjectLifecyclePhase::Alive) ||
        (health && health->effectivelyDead)) {
        return std::nullopt;
    }
    return TargetCandidate{
        .object = object,
        .entity = *entity,
        .position = readAuthoritativeObjectPosition(registry, *entity,
                                                     *transform),
    };
}

} // namespace

void ObjectCleanupHazardSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectCleanupHazardPlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->cleanupHazardPlan
            : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectCleanupHazardComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    for (ObjectCleanupHazardRuntime& runtime : component.instances) {
        runtime.nextScanTick = confirmedTick;
    }
    if (ObjectCleanupHazardComponent* existing =
            ecs::try_get<ObjectCleanupHazardComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectCleanupHazardComponent>(registry, entity,
                                                   std::move(component));
    }
}

bool ObjectCleanupHazardSystem::activateArea(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& center, Fixed moveRange,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick) const {
    static_cast<void>(logicFramesPerSecond);
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return false;
    ObjectCleanupHazardComponent* component =
        ecs::try_get<ObjectCleanupHazardComponent>(registry, *entity);
    if (!component || !component->plan || component->instances.empty()) {
        return false;
    }

    ObjectCleanupHazardRuntime& runtime = component->instances.front();
    runtime.areaCenter = center;
    runtime.moveRange = std::max(Fixed{}, moveRange);
    runtime.areaActive = true;
    // CleanupHazardUpdate::setCleanupAreaParameters changes only m_pos,
    // m_moveRange and the AI move intent.  The currently tracked hazard and
    // m_nextScanFrames survive the command, so an ambulance can keep firing
    // while transitioning into area-cleanup mode instead of waiting through
    // an artificial full ScanRate delay.
    runtime.lastUpdateTick = UINT64_MAX;

    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, *entity);
    }
    runtime.observedExternalOrderRevision = queue->externalRevision;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
    replaceCleanupOrder(*queue, runtime, 0,
                        owner ? owner->player : INVALID_PLAYER_ID,
                        ObjectOrderKind::Move, INVALID_OBJECT_ID, center,
                        confirmedTick);
    return true;
}

void ObjectCleanupHazardSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, const ObjectSimulationRules& rules,
    SimulationRandom* random, uint64_t confirmedTick) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectCleanupHazardComponent>(registry);
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

    for (const Candidate& candidate : candidates) {
        if (unavailable(registry, lifecycle, candidate.entity,
                        candidate.object) ||
            isObjectDisabled(registry, candidate.entity, confirmedTick) ||
            ecs::try_get<ObjectContainedByComponent>(registry,
                                                      candidate.entity)) {
            continue;
        }
        ObjectCleanupHazardComponent& component =
            ecs::get<ObjectCleanupHazardComponent>(registry,
                                                   candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        if (!transform) continue;
        const LogicFixedVec3 selfPosition = readAuthoritativeObjectPosition(
            registry, candidate.entity, *transform);

        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectCleanupHazardRuntime& runtime = component.instances[index];
            const game::ObjectCleanupHazardRule& rule =
                component.plan->rules[index];
            if (runtime.lastUpdateTick == confirmedTick) continue;
            runtime.lastUpdateTick = confirmedTick;

            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                        candidate.entity);
            if (queue && !queue->orders.empty() &&
                !isCleanupOrder(queue->orders.front(), index)) {
                // A player/script (or a different autonomous module) owns the
                // active intent.  Explicit input abandons area mode exactly as
                // RefCode checks CMD_FROM_AI; passive cleanup simply sleeps.
                if (queue->orders.front().source != ObjectOrderSource::System) {
                    runtime.areaActive = false;
                    runtime.moveRange = {};
                    runtime.bestTarget = INVALID_OBJECT_ID;
                    runtime.inWeaponRange = false;
                    releaseCleanupLock(registry, candidate.entity, runtime);
                }
                continue;
            }
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    registry, candidate.entity);
            }
            if (queue->externalRevision !=
                runtime.observedExternalOrderRevision) {
                runtime.observedExternalOrderRevision =
                    queue->externalRevision;
                if (runtime.areaActive) {
                    clearCleanupOrder(queue, index);
                    runtime.areaActive = false;
                    runtime.moveRange = {};
                    runtime.bestTarget = INVALID_OBJECT_ID;
                    runtime.inWeaponRange = false;
                    releaseCleanupLock(registry, candidate.entity, runtime);
                }
                continue;
            }

            std::optional<TargetCandidate> target = resolveHazard(
                registry, lifecycle, candidate.entity, runtime.bestTarget);
            const bool scanDue = confirmedTick >= runtime.nextScanTick;
            if (scanDue) {
                const LogicFixedVec3 center = runtime.areaActive
                    ? runtime.areaCenter
                    : selfPosition;
                const Fixed radius = runtime.areaActive
                    ? saturatingFixedAdd(rule.scanRange, runtime.moveRange)
                    : rule.scanRange;
                target = closestHazard(registry, lifecycle, candidate.entity,
                                       center, radius);
                runtime.bestTarget = target ? target->object
                                            : INVALID_OBJECT_ID;
                const uint64_t scanTicks = millisecondsToTicks(
                    rule.scanRateMilliseconds, rules.logicFramesPerSecond);
                // Preserve the authored duration rather than the old
                // decrement-before-test N+1 callback artifact.
                runtime.nextScanTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(1u, scanTicks));
            }

            if (!target) {
                // Between periodic scans RefCode only tries to fire the
                // tracked ObjectId.  If it vanished, preserve the current
                // center move/idle state until the next authored scan rather
                // than manufacturing an early area-complete transition.
                if (!scanDue) continue;
                runtime.inWeaponRange = false;
                releaseCleanupLock(registry, candidate.entity, runtime);
                if (!runtime.areaActive) {
                    clearCleanupOrder(queue, index);
                    continue;
                }

                constexpr Fixed kAreaCompleteDistance{int32_t{25}};
                if (within2D(selfPosition, runtime.areaCenter,
                             kAreaCompleteDistance)) {
                    clearCleanupOrder(queue, index);
                    runtime.areaActive = false;
                    runtime.moveRange = {};
                    continue;
                }
                replaceCleanupOrder(
                    *queue, runtime, index,
                    owner ? owner->player : INVALID_PLAYER_ID,
                    ObjectOrderKind::Move, INVALID_OBJECT_ID,
                    runtime.areaCenter, confirmedTick);
                continue;
            }

            const std::optional<Fixed> weaponRange = cleanupWeaponRange(
                registry, candidate.entity, content, rule.weaponSlot,
                rules.logicFramesPerSecond, confirmedTick);
            if (!weaponRange) {
                clearCleanupOrder(queue, index);
                releaseCleanupLock(registry, candidate.entity, runtime);
                continue;
            }
            const bool inRange = within2D(selfPosition, target->position,
                                          *weaponRange);
            if (!inRange) {
                // If a tracked passive hazard moved away after entering range,
                // force a short re-evaluation like RefCode's 0..3 frame scan.
                if (!runtime.areaActive && runtime.inWeaponRange) {
                    runtime.bestTarget = INVALID_OBJECT_ID;
                    const uint64_t jitter = random
                        ? static_cast<uint64_t>(
                              random->integerInclusive(0, 3))
                        : 0u;
                    runtime.nextScanTick = saturatingAdd(confirmedTick, jitter);
                    runtime.inWeaponRange = false;
                    clearCleanupOrder(queue, index);
                    releaseCleanupLock(registry, candidate.entity, runtime);
                    continue;
                }
                runtime.inWeaponRange = false;
                releaseCleanupLock(registry, candidate.entity, runtime);
                replaceCleanupOrder(
                    *queue, runtime, index,
                    owner ? owner->player : INVALID_PLAYER_ID,
                    ObjectOrderKind::Move, INVALID_OBJECT_ID,
                    pursuitPosition(selfPosition, target->position,
                                    *weaponRange),
                    confirmedTick);
                continue;
            }

            runtime.inWeaponRange = true;
            if (setObjectWeaponLock(registry, candidate.entity,
                                    rule.weaponSlot,
                                    ObjectWeaponLockType::Temporary)) {
                runtime.ownsTemporaryWeaponLock = true;
            }
            replaceCleanupOrder(
                *queue, runtime, index,
                owner ? owner->player : INVALID_PLAYER_ID,
                ObjectOrderKind::Attack, target->object, target->position,
                confirmedTick);
        }
    }
}

} // namespace engine

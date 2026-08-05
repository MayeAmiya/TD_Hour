#include "game/object/simulation/combat/ObjectCountermeasures.h"

#include "core/container/string_utils.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

namespace engine {
namespace {

using Fixed = math::q32_32;

[[nodiscard]] uint32_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t rate) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t ticks =
        (static_cast<uint64_t>(milliseconds) * std::max(1u, rate) + 999u) /
        1000u;
    return static_cast<uint32_t>(std::min<uint64_t>(ticks, UINT32_MAX));
}

[[nodiscard]] uint64_t addTicks(uint64_t tick,
                                uint64_t delay) noexcept {
    return delay > UINT64_MAX - tick ? UINT64_MAX : tick + delay;
}

[[nodiscard]] uint32_t totalFlares(
    const game::ObjectCountermeasuresRule& rule) noexcept {
    return static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(rule.volleySize) * rule.numberOfVolleys,
        UINT32_MAX));
}

[[nodiscard]] bool containsKind(const ObjectKindOfComponent* kinds,
                                game::ObjectKindOf token) noexcept {
    return kinds && game::objectHasKind(kinds->mask, token);
}

[[nodiscard]] bool probabilityRoll(SimulationRandom& random,
                                   Fixed probability) noexcept {
    probability = Fixed::clamp(probability, {}, Fixed{int32_t{1}});
    // Q32.32 already expresses the exact [0, 2^32] threshold for a 32-bit
    // uniform draw. Using UINT32_MAX as a scale made authored 100% fail for
    // the single maximum RNG value.
    const uint64_t threshold = static_cast<uint64_t>(probability.raw());
    return static_cast<uint64_t>(random.nextUInt32()) < threshold;
}

[[nodiscard]] LogicFixedVec3 fixedPosition(
    const ecs::registry& registry, ecs::entity entity,
    const TransformComponent& value) noexcept {
    return readAuthoritativeObjectPosition(registry, entity, value);
}

[[nodiscard]] Fixed distanceSquared2D(
    const ecs::registry& registry,
    ecs::entity leftEntity, const TransformComponent& left,
    ecs::entity rightEntity, const TransformComponent& right) noexcept {
    const LogicFixedVec3 leftPosition = fixedPosition(
        registry, leftEntity, left);
    const LogicFixedVec3 rightPosition = fixedPosition(
        registry, rightEntity, right);
    const Fixed dx = leftPosition.x - rightPosition.x;
    const Fixed dy = leftPosition.y - rightPosition.y;
    return dx * dx + dy * dy;
}

} // namespace

void ObjectCountermeasuresSystem::reset() noexcept {
    container::Vector<ObjectCountermeasureFlareSpawnCommand>{}.swap(
        m_spawnCommands);
    container::Vector<ObjectCountermeasureEvent>{}.swap(m_events);
}

void ObjectCountermeasuresSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    uint32_t logicFramesPerSecond) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!objectTemplate || !objectTemplate->archetype ||
        !objectTemplate->archetype->countermeasuresPlan) return;
    ObjectCountermeasuresComponent component;
    component.plan = objectTemplate->archetype->countermeasuresPlan;
    component.rules.resize(component.plan->rules.size());
    for (size_t index = 0; index < component.rules.size(); ++index) {
        const game::ObjectCountermeasuresRule& authored =
            component.plan->rules[index];
        ObjectCountermeasureRuleRuntime& runtime = component.rules[index];
        runtime.availableFlares = totalFlares(authored);
        runtime.delayBetweenVolleysTicks = millisecondsToTicks(
            authored.delayBetweenVolleysMilliseconds, logicFramesPerSecond);
        runtime.reloadTicks = millisecondsToTicks(
            authored.reloadMilliseconds, logicFramesPerSecond);
        runtime.missileDecoyTicks = millisecondsToTicks(
            authored.missileDecoyMilliseconds, logicFramesPerSecond);
        runtime.reactionLatencyTicks = millisecondsToTicks(
            authored.reactionLaunchLatencyMilliseconds,
            logicFramesPerSecond);
    }
    if (ObjectCountermeasuresComponent* existing =
            ecs::try_get<ObjectCountermeasuresComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectCountermeasuresComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectCountermeasuresSystem::reevaluateObject(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object,
    const UpgradeMask& playerCompletedUpgrades,
    uint64_t confirmedTick,
    const UpgradeCatalog* catalog) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return;
    ObjectCountermeasuresComponent* component =
        ecs::try_get<ObjectCountermeasuresComponent>(registry, *entity);
    if (!component || !component->plan) return;
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, *entity);
    const UpgradeMask local = inventory
        ? inventory->completed : UpgradeMask{};
    const size_t count = std::min(component->rules.size(),
                                  component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectCountermeasuresRule& authored =
            component->plan->rules[index];
        ObjectCountermeasureRuleRuntime& runtime = component->rules[index];
        const bool active = game::objectFireWeaponUpgradeMatches(
            authored.upgradeMux, playerCompletedUpgrades, local, catalog) &&
            !game::objectFireWeaponUpgradeHasConflict(
                authored.upgradeMux, playerCompletedUpgrades, local, catalog);
        if (active == runtime.upgradeActive) continue;
        runtime.upgradeActive = active;
        runtime.reactionDueTick = 0;
        runtime.nextVolleyDueTick = 0;
        runtime.reloadDueTick = 0;
        runtime.phase = active ? ObjectCountermeasurePhase::Idle
                               : ObjectCountermeasurePhase::Inactive;
        static_cast<void>(confirmedTick);
    }
}

bool ObjectCountermeasuresSystem::reportIncomingMissile(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId victim, ObjectId projectile, SimulationRandom& random,
    uint64_t confirmedTick) {
    const std::optional<ecs::entity> victimEntity =
        lifecycle.entityFromId(victim);
    const std::optional<ecs::entity> projectileEntity =
        lifecycle.entityFromId(projectile);
    if (!victimEntity || !projectileEntity) return false;
    const ThingTemplateComponent* victimTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, *victimEntity);
    const ObjectKindOfComponent* projectileKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *projectileEntity);
    ObjectCountermeasuresComponent* component =
        ecs::try_get<ObjectCountermeasuresComponent>(registry, *victimEntity);
    ObjectProjectileComponent* projectileState =
        ecs::try_get<ObjectProjectileComponent>(registry, *projectileEntity);
    ObjectMissileProjectileComponent* missile =
        ecs::try_get<ObjectMissileProjectileComponent>(registry,
                                                        *projectileEntity);
    if (!victimTemplate || !victimTemplate->archetype ||
        !victimTemplate->archetype->hasAiUpdate ||
        !containsKind(projectileKinds, game::ObjectKindOf::SmallMissile) ||
        !component ||
        !component->plan || !projectileState || !missile ||
        projectileState->intendedTarget != victim) {
        return false;
    }
    const size_t count = std::min(component->rules.size(),
                                  component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectCountermeasureRuleRuntime& runtime = component->rules[index];
        if (!runtime.upgradeActive) continue;
        const game::ObjectCountermeasuresRule& authored =
            component->plan->rules[index];
        ++runtime.incomingMissiles;
        m_events.push_back({
            .kind = ObjectCountermeasureEventKind::ThreatReported,
            .source = victim,
            .projectile = projectile,
            .authoredOrder = authored.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        if (runtime.availableFlares + runtime.flares.size() == 0 ||
            !probabilityRoll(random, authored.evasionRate)) {
            return false;
        }
        missile->countermeasureVictim = victim;
        missile->countermeasureDiversionTick =
            addTicks(confirmedTick, runtime.missileDecoyTicks);
        missile->countermeasureRuleIndex = static_cast<uint32_t>(index);
        missile->countermeasureDiversionPending = true;
        ++runtime.divertedMissiles;
        if (runtime.flares.empty() &&
            runtime.phase != ObjectCountermeasurePhase::AwaitReaction) {
            runtime.reactionDueTick =
                addTicks(confirmedTick, runtime.reactionLatencyTicks);
            runtime.phase = ObjectCountermeasurePhase::AwaitReaction;
        }
        m_events.push_back({
            .kind = ObjectCountermeasureEventKind::DiversionScheduled,
            .source = victim,
            .projectile = projectile,
            .authoredOrder = authored.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        return true; // author-first interface policy
    }
    return false;
}

void ObjectCountermeasuresSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectCountermeasuresComponent,
                                TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        if (identity.id && lifecycle.entityFromId(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectCountermeasuresComponent& component =
            ecs::get<ObjectCountermeasuresComponent>(registry,
                                                      candidate.entity);
        if (!component.plan) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        if ((health && health->effectivelyDead) ||
            isObjectDisabled(registry, candidate.entity, confirmedTick,
                objectDisabledBit(ObjectDisabledReason::Held))) {
            continue;
        }
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, candidate.entity);
        const bool isAirborne =
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::AirborneTarget))) ||
            (airborne && airborne->isAirborne);
        const TransformComponent& transform =
            ecs::get<TransformComponent>(registry, candidate.entity);
        LogicFixedVec3 inheritedVelocity;
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                     candidate.entity)) {
            inheritedVelocity = physics->velocityUnitsPerSecond;
        }
        const Fixed speed = Fixed::sqrt(
            inheritedVelocity.x * inheritedVelocity.x +
            inheritedVelocity.y * inheritedVelocity.y +
            inheritedVelocity.z * inheritedVelocity.z);
        const size_t count = std::min(component.rules.size(),
                                      component.plan->rules.size());
        for (size_t index = 0; index < count; ++index) {
            ObjectCountermeasureRuleRuntime& runtime = component.rules[index];
            const game::ObjectCountermeasuresRule& authored =
                component.plan->rules[index];
            runtime.flares.erase(std::remove_if(
                runtime.flares.begin(), runtime.flares.end(),
                [&](ObjectId flare) {
                    return !lifecycle.entityFromId(flare);
                }), runtime.flares.end());
            if (!runtime.upgradeActive) continue;

            if (runtime.availableFlares == 0 &&
                runtime.pendingFlareSpawns == 0) {
                if (runtime.reloadTicks == 0) {
                    runtime.phase = ObjectCountermeasurePhase::EmptyManual;
                } else if (runtime.reloadDueTick == 0) {
                    runtime.reloadDueTick = addTicks(
                        confirmedTick, runtime.reloadTicks);
                    runtime.phase = ObjectCountermeasurePhase::Reloading;
                } else if (confirmedTick >= runtime.reloadDueTick) {
                    runtime.availableFlares = totalFlares(authored);
                    runtime.reloadDueTick = 0;
                    runtime.phase = ObjectCountermeasurePhase::Idle;
                    m_events.push_back({
                        .kind = ObjectCountermeasureEventKind::Reloaded,
                        .source = candidate.object,
                        .authoredOrder = authored.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            if (!isAirborne || runtime.availableFlares == 0) continue;
            const bool due =
                (runtime.phase == ObjectCountermeasurePhase::AwaitReaction &&
                 confirmedTick >= runtime.reactionDueTick) ||
                (runtime.phase == ObjectCountermeasurePhase::FiringSequence &&
                 confirmedTick >= runtime.nextVolleyDueTick);
            if (!due || runtime.pendingFlareSpawns != 0) continue;

            const uint32_t volley = std::min(authored.volleySize,
                                              runtime.availableFlares);
            if (volley == 0 || authored.flareTemplate.empty()) continue;
            ++runtime.launchSequence;
            if (runtime.launchSequence == 0) ++runtime.launchSequence;
            runtime.pendingFlareSpawns = volley;
            runtime.reactionDueTick = 0;
            runtime.nextVolleyDueTick =
                addTicks(confirmedTick, runtime.delayBetweenVolleysTicks);
            runtime.phase = ObjectCountermeasurePhase::FiringSequence;
            const Fixed yaw = readAuthoritativeObjectYaw(
                registry, candidate.entity, transform);
            const Fixed scalar = speed < Fixed{int32_t{1}}
                ? Fixed{int32_t{-10}} : speed;
            for (uint32_t ordinal = 0; ordinal < volley; ++ordinal) {
                Fixed ratio{};
                if (volley != 1) {
                    ratio = Fixed{static_cast<int32_t>(ordinal)} /
                        Fixed{static_cast<int32_t>(volley - 1)} *
                        Fixed{int32_t{2}} - Fixed{int32_t{1}};
                }
                const math::q32_32_sincos direction = math::fixed_sincos(
                    yaw + ratio * authored.volleyArcRadians);
                const Fixed motiveScale =
                    scalar * authored.volleyVelocityFactor;
                m_spawnCommands.push_back({
                    .source = candidate.object,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = authored.authoredOrder,
                    .launchSequence = runtime.launchSequence,
                    .flareOrdinal = ordinal,
                    .flareTemplate = authored.flareTemplate,
                    .position = fixedPosition(
                        registry, candidate.entity, transform),
                    .orientationRadians = yaw,
                    .inheritedVelocityUnitsPerSecond = inheritedVelocity,
                    .motiveForce = {
                        direction.cosine * motiveScale,
                        direction.sine * motiveScale,
                        {},
                    },
                    .confirmedTick = confirmedTick,
                });
            }
            m_events.push_back({
                .kind = ObjectCountermeasureEventKind::VolleyLaunched,
                .source = candidate.object,
                .authoredOrder = authored.authoredOrder,
                .sequence = runtime.launchSequence,
                .confirmedTick = confirmedTick,
            });
        }
    }
}

void ObjectCountermeasuresSystem::acknowledgeFlareSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, uint32_t ruleIndex, ObjectId flare, bool created,
    uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(source);
    if (!entity) return;
    ObjectCountermeasuresComponent* component =
        ecs::try_get<ObjectCountermeasuresComponent>(registry, *entity);
    if (!component || !component->plan ||
        ruleIndex >= component->rules.size() ||
        ruleIndex >= component->plan->rules.size()) return;
    ObjectCountermeasureRuleRuntime& runtime = component->rules[ruleIndex];
    if (runtime.pendingFlareSpawns != 0) --runtime.pendingFlareSpawns;
    if (created && flare && runtime.availableFlares != 0) {
        --runtime.availableFlares;
        runtime.flares.push_back(flare);
    }
    if (runtime.pendingFlareSpawns == 0 && runtime.availableFlares == 0) {
        runtime.reloadDueTick = runtime.reloadTicks == 0
            ? 0 : addTicks(confirmedTick, runtime.reloadTicks);
        runtime.phase = runtime.reloadTicks == 0
            ? ObjectCountermeasurePhase::EmptyManual
            : ObjectCountermeasurePhase::Reloading;
    }
}

void ObjectCountermeasuresSystem::resolveMissileDiversions(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> missiles;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectProjectileComponent,
                                ObjectMissileProjectileComponent>(registry);
    missiles.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        const ObjectMissileProjectileComponent& missile =
            view.template get<ObjectMissileProjectileComponent>(entity);
        if (identity.id && missile.countermeasureDiversionPending &&
            confirmedTick >= missile.countermeasureDiversionTick &&
            lifecycle.entityFromId(identity.id)) {
            missiles.push_back({identity.id, entity});
        }
    }
    std::sort(missiles.begin(), missiles.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    for (const Candidate& candidate : missiles) {
        ObjectMissileProjectileComponent& missile =
            ecs::get<ObjectMissileProjectileComponent>(registry,
                                                        candidate.entity);
        ObjectProjectileComponent& projectile =
            ecs::get<ObjectProjectileComponent>(registry, candidate.entity);
        missile.countermeasureDiversionPending = false;
        missile.suppressDetonationDamage = true;
        const std::optional<ecs::entity> victim =
            lifecycle.entityFromId(missile.countermeasureVictim);
        ObjectCountermeasuresComponent* counter = victim
            ? ecs::try_get<ObjectCountermeasuresComponent>(registry, *victim)
            : nullptr;
        const TransformComponent* victimTransform = victim
            ? ecs::try_get<TransformComponent>(registry, *victim) : nullptr;
        const size_t ruleIndex = missile.countermeasureRuleIndex;
        if (!counter || !counter->plan || !victimTransform ||
            ruleIndex >= counter->rules.size() ||
            ruleIndex >= counter->plan->rules.size()) {
            continue;
        }
        const game::ObjectCountermeasuresRule& authored =
            counter->plan->rules[ruleIndex];
        ObjectCountermeasureRuleRuntime& runtime = counter->rules[ruleIndex];
        ObjectId selected = INVALID_OBJECT_ID;
        Fixed best{};
        uint32_t counted = 0;
        for (auto iterator = runtime.flares.rbegin();
             iterator != runtime.flares.rend() &&
             counted < std::max(1u, authored.volleySize); ++iterator) {
            const std::optional<ecs::entity> flareEntity =
                lifecycle.entityFromId(*iterator);
            if (!flareEntity) continue;
            const TransformComponent* flareTransform =
                ecs::try_get<TransformComponent>(registry, *flareEntity);
            if (!flareTransform) continue;
            ++counted;
            const Fixed distance = distanceSquared2D(
                registry, *flareEntity, *flareTransform,
                *victim, *victimTransform);
            if (!selected || distance < best) {
                selected = *iterator;
                best = distance;
            }
        }
        if (!selected) continue;
        const std::optional<ecs::entity> flareEntity =
            lifecycle.entityFromId(selected);
        const TransformComponent* flareTransform = flareEntity
            ? ecs::try_get<TransformComponent>(registry, *flareEntity)
            : nullptr;
        if (!flareTransform) continue;
        projectile.intendedTarget = selected;
        projectile.target = fixedPosition(
            registry, *flareEntity, *flareTransform);
        missile.originalTarget = projectile.target;
        missile.trackingTarget = true;
        missile.divertedToCountermeasure = true;
        m_events.push_back({
            .kind = ObjectCountermeasureEventKind::MissileRetargeted,
            .source = missile.countermeasureVictim,
            .projectile = candidate.object,
            .flare = selected,
            .authoredOrder = authored.authoredOrder,
            .confirmedTick = confirmedTick,
        });
    }
}

bool ObjectCountermeasuresSystem::reload(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    ObjectCountermeasuresComponent* component = entity
        ? ecs::try_get<ObjectCountermeasuresComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan) return false;
    bool changed = false;
    const size_t count = std::min(component->rules.size(),
                                  component->plan->rules.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectCountermeasureRuleRuntime& runtime = component->rules[index];
        if (!runtime.upgradeActive) continue;
        runtime.availableFlares = totalFlares(component->plan->rules[index]);
        runtime.reloadDueTick = 0;
        runtime.phase = ObjectCountermeasurePhase::Idle;
        changed = true;
        m_events.push_back({
            .kind = ObjectCountermeasureEventKind::Reloaded,
            .source = object,
            .authoredOrder = component->plan->rules[index].authoredOrder,
            .confirmedTick = confirmedTick,
        });
    }
    return changed;
}

container::Vector<ObjectCountermeasureFlareSpawnCommand>
ObjectCountermeasuresSystem::takeFlareSpawnCommands() {
    container::Vector<ObjectCountermeasureFlareSpawnCommand> output;
    output.swap(m_spawnCommands);
    return output;
}

void ObjectCountermeasuresSystem::drainFlareSpawnCommands(
    container::Vector<ObjectCountermeasureFlareSpawnCommand>& out) {
    out.clear();
    out.reserve(m_spawnCommands.size());
    for (ObjectCountermeasureFlareSpawnCommand& command : m_spawnCommands) {
        out.push_back(std::move(command));
    }
    m_spawnCommands.clear();
}

void ObjectCountermeasuresSystem::discardFlareSpawnCommands() noexcept {
    m_spawnCommands.clear();
}

container::Vector<ObjectCountermeasureEvent>
ObjectCountermeasuresSystem::takeEvents() {
    container::Vector<ObjectCountermeasureEvent> output;
    output.swap(m_events);
    return output;
}

} // namespace engine

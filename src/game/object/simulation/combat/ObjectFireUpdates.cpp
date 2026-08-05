#include "core/container/string_utils.h"
#include "game/data/base/ContentBoolParsing.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * fps;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

void advanceSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

[[nodiscard]] game::ObjectStatusMask statusMask(
    game::ObjectStatusFlag flag) noexcept {
    return game::objectStatusBit(flag);
}

[[nodiscard]] bool hasStatus(const ecs::registry& registry, ecs::entity entity,
                             game::ObjectStatusFlag flag) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(statusMask(flag));
}

void setModelCondition(ecs::registry& registry, ecs::entity entity,
                       game::ModelConditionFlag flag, bool enabled,
                       uint64_t confirmedTick) {
    if (!ecs::try_get<RenderModelComponent>(registry, entity)) return;
    const game::ModelConditionMask mask = game::modelConditionMaskOf(flag);
    const game::ModelConditionMask empty;
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Fire,
        enabled ? empty : mask, enabled ? mask : empty, confirmedTick);
}

[[nodiscard]] uint32_t chooseDelayMilliseconds(
    uint32_t minimum, uint32_t maximum, SimulationRandom* random) noexcept {
    if (maximum < minimum) maximum = minimum;
    if (!random || minimum == maximum) return minimum;
    const uint32_t boundedMinimum = std::min<uint32_t>(
        minimum, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    const uint32_t boundedMaximum = std::min<uint32_t>(
        maximum, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    return static_cast<uint32_t>(random->integerInclusive(
        static_cast<int32_t>(boundedMinimum),
        static_cast<int32_t>(std::max(boundedMinimum, boundedMaximum))));
}

void armFireSpread(ObjectFireSpreadComponent* spread,
                   const game::ObjectFireSpreadPlan* plan,
                   SimulationRandom* random, uint32_t logicFramesPerSecond,
                   uint64_t confirmedTick) {
    if (!spread || !plan) return;
    const size_t count = std::min(plan->rules.size(), spread->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectFireSpreadParameters& rule = plan->rules[index];
        ObjectFireSpreadRuntime& runtime = spread->instances[index];
        const uint32_t milliseconds = chooseDelayMilliseconds(
            rule.minimumSpreadDelayMilliseconds,
            rule.maximumSpreadDelayMilliseconds, random);
        const uint64_t delay = std::max<uint64_t>(
            1, millisecondsToTicks(milliseconds, logicFramesPerSecond));
        runtime.nextSpreadTick = saturatingAdd(confirmedTick, delay);
        runtime.armed = true;
    }
}

[[nodiscard]] bool canIgnite(const ecs::registry& registry,
                             ecs::entity entity) noexcept {
    if (hasStatus(registry, entity, game::ObjectStatusFlag::Aflame) ||
        hasStatus(registry, entity, game::ObjectStatusFlag::Burned)) {
        return false;
    }
    const ObjectFlammableComponent* component =
        ecs::try_get<ObjectFlammableComponent>(registry, entity);
    return component && std::any_of(
        component->instances.begin(), component->instances.end(),
        [](const ObjectFlammableRuntime& runtime) {
            return runtime.state == ObjectFlammabilityState::Normal;
        });
}

[[nodiscard]] bool igniteInstance(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    size_t instanceIndex, SimulationRandom* random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    ObjectFlammableComponent* component =
        ecs::try_get<ObjectFlammableComponent>(registry, entity);
    if (!component || !component->plan ||
        instanceIndex >= component->instances.size() ||
        instanceIndex >= component->plan->rules.size() ||
        !canIgnite(registry, entity)) return false;
    ObjectFlammableRuntime& runtime = component->instances[instanceIndex];
    if (runtime.state != ObjectFlammabilityState::Normal) return false;
    const game::ObjectFlammableParameters& rule =
        component->plan->rules[instanceIndex];

    runtime.state = ObjectFlammabilityState::Aflame;
    runtime.aflameEndTick = rule.aflameDurationMilliseconds == 0 ? 0
        : saturatingAdd(confirmedTick, millisecondsToTicks(
              rule.aflameDurationMilliseconds, logicFramesPerSecond));
    runtime.burnedTick = rule.burnedDelayMilliseconds == 0 ? 0
        : saturatingAdd(confirmedTick, millisecondsToTicks(
              rule.burnedDelayMilliseconds, logicFramesPerSecond));
    runtime.nextAflameDamageTick =
        rule.aflameDamageDelayMilliseconds == 0 ? 0
        : saturatingAdd(confirmedTick, millisecondsToTicks(
              rule.aflameDamageDelayMilliseconds, logicFramesPerSecond));

    static_cast<void>(ObjectStatusSystem::apply(registry, entity, {
        .setMask = statusMask(game::ObjectStatusFlag::Aflame),
        .confirmedTick = confirmedTick,
    }));
    setModelCondition(registry, entity, game::ModelConditionFlag::Aflame, true,
                      confirmedTick);
    if (!rule.burningSoundName.empty()) {
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StartLoop,
            .object = object,
            .eventName = rule.burningSoundName,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
    }
    ObjectFireSpreadComponent* spread =
        ecs::try_get<ObjectFireSpreadComponent>(registry, entity);
    armFireSpread(spread, spread && spread->plan ? spread->plan.get() : nullptr,
                  random, logicFramesPerSecond, confirmedTick);
    return true;
}

[[nodiscard]] bool igniteFirst(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    SimulationRandom* random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, container::Vector<ObjectFireAudioCommand>& outAudio) {
    ObjectFlammableComponent* component =
        ecs::try_get<ObjectFlammableComponent>(registry, entity);
    if (!component) return false;
    for (size_t index = 0; index < component->instances.size(); ++index) {
        if (component->instances[index].state !=
            ObjectFlammabilityState::Normal) continue;
        return igniteInstance(registry, entity, object, index, random,
                              logicFramesPerSecond, confirmedTick, outAudio);
    }
    return false;
}

[[nodiscard]] UpgradeMask localUpgrades(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    return inventory ? inventory->completed : UpgradeMask{};
}

[[nodiscard]] bool cooldownUpgradeConditionsAllow(
    const game::ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeMask& player,
    const engine::UpgradeMask& object,
    const engine::UpgradeCatalog* catalog) noexcept {
    static_cast<void>(catalog);
    if (!mux.masksCompiled) return false;
    const engine::UpgradeMask completed = player | object;
    if (completed.test_for_any(mux.conflictsWithMask)) return false;
    if (mux.triggeredByMask.none()) return true;
    return mux.requiresAllTriggers
        ? completed.test_for_all(mux.triggeredByMask)
        : completed.test_for_any(mux.triggeredByMask);
}

[[nodiscard]] std::optional<ObjectCreationListInvocation> makeInvocation(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    game::ObjectCreationListContentId contentId,
    const GameContentSnapshot& content, uint32_t authoredOrder,
    uint32_t lifetimeOverrideFrames, uint64_t emissionSequence,
    uint64_t confirmedTick, bool hasSecondaryPosition) {
    if (!contentId || !content.findObjectCreationList(contentId)) {
        return std::nullopt;
    }
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    const PrimaryTeamComponent* team =
        ecs::try_get<PrimaryTeamComponent>(registry, entity);
    if (!transform || !owner || !team) return std::nullopt;
    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, entity, *transform);
    LogicFixedVec3 velocity;
    const ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    if (physics) {
        velocity = physics->velocityUnitsPerSecond;
    }
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    if (const ObjectVeterancyComponent* experience =
            ecs::try_get<ObjectVeterancyComponent>(registry, entity)) {
        veterancy = experience->level;
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    return ObjectCreationListInvocation{
        .content = contentId,
        .source = object,
        .owner = owner->player,
        .primaryTeam = team->team,
        .primaryPosition = position,
        .secondaryPosition = position,
        .sourceVelocity = velocity,
        .orientationRadians = physics && physics->ownsAttitude
            ? physics->yaw
            : readAuthoritativeObjectYaw(registry, entity, *transform),
        .pitchRadians = physics && physics->ownsAttitude
            ? physics->pitch : ObjectPhysicsComponent::Scalar{},
        .rollRadians = physics && physics->ownsAttitude
            ? physics->roll : ObjectPhysicsComponent::Scalar{},
        .veterancy = veterancy,
        .lifetimeOverrideFrames = lifetimeOverrideFrames,
        .authoredOrder = authoredOrder,
        .emissionSequence = emissionSequence,
        .confirmedTick = confirmedTick,
        .sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer,
        .hasSecondaryPosition = hasSecondaryPosition,
        .sourceAirborne = airborne && airborne->isAirborne,
        .sourceOwnsFullAttitude = physics && physics->ownsAttitude,
    };
}

[[nodiscard]] uint32_t lifetimeOverrideFrames(
    const game::ObjectFireOclAfterCooldownParameters& rule,
    uint64_t startTick, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond) noexcept {
    const uint64_t elapsed = confirmedTick >= startTick
        ? confirmedTick - startTick : 0;
    const uint64_t product =
        rule.oclLifetimePerSecondMilliseconds != 0 &&
        elapsed > std::numeric_limits<uint64_t>::max() /
                      rule.oclLifetimePerSecondMilliseconds
            ? std::numeric_limits<uint64_t>::max()
            : elapsed * rule.oclLifetimePerSecondMilliseconds;
    uint64_t result = product / 1000u;
    const uint64_t cap = rule.hasAuthoredLifetimeMaximum
        ? millisecondsToTicks(rule.oclLifetimeMaximumMilliseconds,
                              logicFramesPerSecond)
        : 1000u;
    result = std::min(result, cap);
    return static_cast<uint32_t>(std::min<uint64_t>(
        result, std::numeric_limits<uint32_t>::max()));
}

} // namespace

void ObjectFireUpdateSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!objectTemplate || !objectTemplate->archetype) return;

    if (const auto& plan = objectTemplate->archetype->flammablePlan;
        plan && !plan->rules.empty()) {
        ObjectFlammableComponent component;
        component.plan = plan;
        component.instances.resize(plan->rules.size());
        for (size_t index = 0; index < plan->rules.size(); ++index) {
            component.instances[index].remainingFlameDamage =
                plan->rules[index].flameDamageLimit;
        }
        ecs::emplace<ObjectFlammableComponent>(registry, entity,
                                                std::move(component));
    }
    if (const auto& plan = objectTemplate->archetype->fireSpreadPlan;
        plan && !plan->rules.empty()) {
        ObjectFireSpreadComponent component;
        component.plan = plan;
        component.instances.resize(plan->rules.size());
        for (size_t index = 0; index < plan->rules.size(); ++index) {
            component.instances[index].embersContent =
                content.findObjectCreationListId(
                    plan->rules[index].embersObjectCreationList);
        }
        ecs::emplace<ObjectFireSpreadComponent>(
            registry, entity, std::move(component));
    }
    if (const auto& plan =
            objectTemplate->archetype->fireOclAfterCooldownPlan;
        plan && !plan->rules.empty()) {
        ObjectFireOclAfterCooldownComponent component;
        component.plan = plan;
        component.instances.resize(plan->rules.size());
        for (size_t index = 0; index < plan->rules.size(); ++index) {
            component.instances[index].content =
                content.findObjectCreationListId(
                    plan->rules[index].objectCreationList);
        }
        ecs::emplace<ObjectFireOclAfterCooldownComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectFireUpdateSystem::onHealthEvent(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event, SimulationRandom* random,
    uint32_t logicFramesPerSecond,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    if (!event.object || event.kind != ObjectHealthEventKind::Damaged ||
        (event.damageType != game::DamageType::FLAME &&
         event.damageType != game::DamageType::PARTICLE_BEAM) ||
        event.actualDamageDealtFixed <= math::q32_32{}) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    ObjectFlammableComponent* component =
        ecs::try_get<ObjectFlammableComponent>(registry, *entity);
    if (!component || !component->plan) return;
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectFlammableParameters& rule =
            component->plan->rules[index];
        ObjectFlammableRuntime& runtime = component->instances[index];
        const uint64_t expiration = millisecondsToTicks(
            rule.flameDamageExpirationMilliseconds,
            logicFramesPerSecond);
        if (runtime.hasReceivedFlameDamage &&
            event.confirmedTick > saturatingAdd(
                runtime.lastFlameDamageTick, expiration)) {
            runtime.remainingFlameDamage = rule.flameDamageLimit;
        }
        runtime.lastFlameDamageTick = event.confirmedTick;
        runtime.hasReceivedFlameDamage = true;
        runtime.flameSource = event.source;

        if (!canIgnite(registry, *entity) ||
            runtime.state != ObjectFlammabilityState::Normal) continue;
        runtime.remainingFlameDamage -= event.actualDamageDealtFixed;
        if (runtime.remainingFlameDamage > math::q32_32{}) continue;
        static_cast<void>(igniteInstance(
            registry, *entity, event.object, index, random,
            logicFramesPerSecond, event.confirmedTick, outAudio));
        break;
    }
}

void ObjectFireUpdateSystem::updateFlammable(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectFlammableComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id) &&
            !isObjectDisabled(registry, entity, confirmedTick)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectFlammableComponent& component =
            ecs::get<ObjectFlammableComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectFlammableParameters& rule =
                component.plan->rules[index];
            ObjectFlammableRuntime& runtime = component.instances[index];
            if (runtime.state != ObjectFlammabilityState::Aflame) continue;

            if (runtime.nextAflameDamageTick != 0 &&
                confirmedTick >= runtime.nextAflameDamageTick) {
                const uint64_t delay = std::max<uint64_t>(
                    1, millisecondsToTicks(
                           rule.aflameDamageDelayMilliseconds,
                           logicFramesPerSecond));
                runtime.nextAflameDamageTick = saturatingAdd(
                    confirmedTick, delay);
                if (rule.aflameDamageAmount != 0) {
                    outDamage.push_back({
                        .target = candidate.object,
                        .source = runtime.flameSource,
                        .sourceSequence = rule.authoredOrder,
                        .amount = math::q32_32{
                            rule.aflameDamageAmount},
                        .damageType = game::DamageType::FLAME,
                        .deathType = game::DeathType::BURNED,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            if (runtime.burnedTick != 0 &&
                confirmedTick >= runtime.burnedTick) {
                runtime.burnedTick = 0;
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, candidate.entity, {
                        .setMask = statusMask(game::ObjectStatusFlag::Burned),
                        .confirmedTick = confirmedTick,
                    }));
                setModelCondition(registry, candidate.entity,
                                  game::ModelConditionFlag::Smoldering, true,
                                  confirmedTick);
            }
            if (runtime.aflameEndTick == 0 ||
                confirmedTick < runtime.aflameEndTick) continue;
            runtime.aflameEndTick = 0;
            runtime.nextAflameDamageTick = 0;
            runtime.state = hasStatus(registry, candidate.entity,
                                      game::ObjectStatusFlag::Burned)
                ? ObjectFlammabilityState::Burned
                : ObjectFlammabilityState::Normal;
            static_cast<void>(ObjectStatusSystem::apply(
                registry, candidate.entity, {
                    .clearMask = statusMask(game::ObjectStatusFlag::Aflame),
                    .confirmedTick = confirmedTick,
                }));
            setModelCondition(registry, candidate.entity,
                              game::ModelConditionFlag::Aflame, false,
                              confirmedTick);
            if (!rule.burningSoundName.empty()) {
                outAudio.push_back({
                    .kind = ObjectFireAudioCommandKind::StopLoop,
                    .object = candidate.object,
                    .eventName = rule.burningSoundName,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
        }
    }
}

void ObjectFireUpdateSystem::updateSpread(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectCreationListInvocation>& outInvocations,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> sources;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectFireSpreadComponent,
                                const TransformComponent>(registry);
    sources.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            isObjectDisabled(registry, entity, confirmedTick) ||
            !hasStatus(registry, entity, game::ObjectStatusFlag::Aflame)) {
            continue;
        }
        sources.push_back({identity.id, entity});
    }
    std::sort(sources.begin(), sources.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    if (sources.empty()) return;
    m_currentPositionIndex.rebuild(registry, lifecycle);

    container::Vector<ObjectId> nearby;
    for (const Candidate& source : sources) {
        ObjectFireSpreadComponent& component =
            ecs::get<ObjectFireSpreadComponent>(registry, source.entity);
        if (!component.plan) continue;
        const TransformComponent& sourceTransform =
            ecs::get<const TransformComponent>(registry, source.entity);
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectFireSpreadParameters& rule =
                component.plan->rules[index];
            ObjectFireSpreadRuntime& runtime = component.instances[index];
            if (!runtime.armed || confirmedTick < runtime.nextSpreadTick) {
                continue;
            }
            const uint32_t nextDelayMilliseconds = chooseDelayMilliseconds(
                rule.minimumSpreadDelayMilliseconds,
                rule.maximumSpreadDelayMilliseconds, &random);
            runtime.nextSpreadTick = saturatingAdd(
                confirmedTick, std::max<uint64_t>(
                    1, millisecondsToTicks(nextDelayMilliseconds,
                                           logicFramesPerSecond)));

            if (std::optional<ObjectCreationListInvocation> invocation =
                    makeInvocation(
                        registry, source.entity, source.object,
                        runtime.embersContent, content, rule.authoredOrder, 0,
                        nextEmissionSequence, confirmedTick, false)) {
                outInvocations.push_back(std::move(*invocation));
                advanceSequence(nextEmissionSequence);
            }

            const math::q32_32 range = math::q32_32::max(
                math::q32_32{}, rule.spreadTryRange);
            if (range <= math::q32_32{}) continue;
            const LogicFixedVec3 sourcePosition =
                readAuthoritativeObjectPosition(
                    registry, source.entity, sourceTransform);
            m_currentPositionIndex.queryRadiusFixed(
                sourcePosition, range, nearby);
            ObjectId closest = INVALID_OBJECT_ID;
            math::q32_32 closestDistanceSquared =
                math::q32_32::from_raw(
                    std::numeric_limits<int64_t>::max());
            for (const ObjectId object : nearby) {
                if (!object || object == source.object) continue;
                const std::optional<ecs::entity> target =
                    lifecycle.entityFromId(object);
                if (!target || !canIgnite(registry, *target)) continue;
                const TransformComponent* transform =
                    ecs::try_get<TransformComponent>(registry, *target);
                if (!transform) continue;
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        registry, *target, *transform);
                const math::q32_32 dx =
                    targetPosition.x - sourcePosition.x;
                const math::q32_32 dy =
                    targetPosition.y - sourcePosition.y;
                const math::q32_32 dz =
                    targetPosition.z - sourcePosition.z;
                const math::q32_32 distanceSquared =
                    dx * dx + dy * dy + dz * dz;
                if (distanceSquared > range * range) continue;
                if (!closest || distanceSquared < closestDistanceSquared ||
                    (distanceSquared == closestDistanceSquared &&
                     object < closest)) {
                    closest = object;
                    closestDistanceSquared = distanceSquared;
                }
            }
            if (!closest) continue;
            const std::optional<ecs::entity> target =
                lifecycle.entityFromId(closest);
            if (target) {
                static_cast<void>(igniteFirst(
                    registry, *target, closest, &random,
                    logicFramesPerSecond, confirmedTick, outAudio));
            }
        }
    }
}

void ObjectFireUpdateSystem::updateOclAfterCooldown(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectCreationListInvocation>& outInvocations) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectFireOclAfterCooldownComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id) &&
            !isObjectDisabled(registry, entity, confirmedTick)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectFireOclAfterCooldownComponent& component =
            ecs::get<ObjectFireOclAfterCooldownComponent>(registry,
                                                           candidate.entity);
        const ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(registry, candidate.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const PlayerState* player = owner ? players.get(owner->player) : nullptr;
        const engine::UpgradeMask playerCompleted =
            player ? player->upgrades.completed : engine::UpgradeMask{};
        const engine::UpgradeCatalog* upgradeCatalog = content.upgradeCatalog();
        const engine::UpgradeMask objectCompleted =
            localUpgrades(registry, candidate.entity);
        const ObjectWeaponSetRuntime* activeSet = nullptr;
        if (weapons && weapons->activeWeaponSetIndex &&
            *weapons->activeWeaponSetIndex < weapons->sets.size()) {
            activeSet = &weapons->sets[*weapons->activeWeaponSetIndex];
        }

        const size_t count = component.plan
            ? std::min(component.plan->rules.size(),
                       component.instances.size()) : 0;
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectFireOclAfterCooldownParameters& rule =
                component.plan->rules[index];
            ObjectFireOclAfterCooldownRuntime& runtime =
                component.instances[index];
            bool validThisFrame = true;
            bool validToFireOcl = true;
            const ObjectWeaponSlotRuntime* currentWeapon = nullptr;
            if (activeSet && weapons->currentSlot &&
                static_cast<size_t>(*weapons->currentSlot) <
                    activeSet->slots.size()) {
                const ObjectWeaponSlotRuntime& slot =
                    activeSet->slots[static_cast<size_t>(*weapons->currentSlot)];
                if (slot.content) currentWeapon = &slot;
            }
            if (!currentWeapon || !weapons->currentSlot ||
                *weapons->currentSlot != rule.weaponSlot) {
                validThisFrame = false;
            }
            if (validThisFrame && !cooldownUpgradeConditionsAllow(
                    rule.upgradeMux, playerCompleted, objectCompleted,
                    upgradeCatalog)) {
                validThisFrame = false;
                validToFireOcl = false;
            }

            const auto resetStats = [&]() {
                runtime.consecutiveShots = 0;
                runtime.startTick = 0;
            };
            const auto fireOcl = [&]() {
                const uint32_t lifetime = lifetimeOverrideFrames(
                    rule, runtime.startTick, confirmedTick,
                    logicFramesPerSecond);
                if (std::optional<ObjectCreationListInvocation> invocation =
                        makeInvocation(
                            registry, candidate.entity, candidate.object,
                            runtime.content, content, rule.authoredOrder,
                            lifetime, nextEmissionSequence, confirmedTick,
                            true)) {
                    outInvocations.push_back(std::move(*invocation));
                    advanceSequence(nextEmissionSequence);
                }
                resetStats();
            };

            if (validThisFrame) {
                uint32_t observedShot = 0;
                if (currentWeapon->lastFireSequence != 0 &&
                    saturatingAdd(currentWeapon->lastFireTick, 1) ==
                        confirmedTick) {
                    observedShot = currentWeapon->lastFireSequence;
                } else if (currentWeapon->previousFireSequence != 0 &&
                           saturatingAdd(currentWeapon->previousFireTick, 1) ==
                               confirmedTick) {
                    observedShot = currentWeapon->previousFireSequence;
                }
                if (observedShot != 0 &&
                    observedShot != runtime.lastObservedShotSequence) {
                    runtime.lastObservedShotSequence = observedShot;
                    ++runtime.consecutiveShots;
                    if (runtime.consecutiveShots == 1) {
                        runtime.startTick = confirmedTick;
                    }
                } else if (currentWeapon->nextReadyTick < confirmedTick &&
                           rule.minimumShotsRequired <=
                               runtime.consecutiveShots) {
                    fireOcl();
                }
            } else if (validToFireOcl && activeSet) {
                const size_t configured = static_cast<size_t>(rule.weaponSlot);
                if (configured < activeSet->slots.size() &&
                    activeSet->slots[configured].content &&
                    rule.minimumShotsRequired <= runtime.consecutiveShots) {
                    fireOcl();
                }
            }

            if (validThisFrame != runtime.valid) {
                runtime.valid = validThisFrame;
                resetStats();
            }
        }
    }
}

} // namespace engine

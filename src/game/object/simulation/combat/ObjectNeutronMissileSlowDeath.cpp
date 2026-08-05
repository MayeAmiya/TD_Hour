#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"

#include "game/base/DamageTypes.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/runtime/ObjectToppleTransaction.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace {
using namespace engine;
using Fixed = math::q32_32;

[[nodiscard]] uint64_t firstTickAfterDuration(Fixed milliseconds,
                                              uint32_t fps) noexcept {
    // RefCode stores parseDurationReal's fractional frame value and tests
    // `currentFrame - activationFrame > delay`.  For an integral frame
    // counter that is floor(durationFrames) + 1, including a zero delay.
    const Fixed frames = milliseconds *
        Fixed{static_cast<int32_t>(std::max<uint32_t>(1, fps))} /
        Fixed{int32_t{1000}};
    return static_cast<uint64_t>(std::max<int32_t>(0, frames.to_int())) + 1u;
}

[[nodiscard]] Fixed squared2D(const LogicFixedVec3& a,
                              const LogicFixedVec3& b) noexcept {
    const Fixed dx = a.x - b.x;
    const Fixed dy = a.y - b.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] LogicFixedVec3 positionOf(const ecs::registry& registry,
                                        ecs::entity entity,
                                        const TransformComponent& transform) {
    return readAuthoritativeObjectPosition(registry, entity, transform);
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

void setBurned(ecs::registry& registry, ecs::entity entity,
               uint64_t confirmedTick) {
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = game::objectStatusBit(game::ObjectStatusFlag::Burned),
         .confirmedTick = confirmedTick}));
    if (RenderModelComponent* render =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        static const game::ModelConditionMask burned =
            game::modelConditionMaskOf(game::ModelConditionFlag::Burned);
        const auto previous = render->modelConditionFlags.words;
        for (size_t i = 0; i < burned.words.size(); ++i) {
            render->modelConditionFlags.words[i] |= burned.words[i];
        }
        if (render->modelConditionFlags.words != previous) {
            // BURNED belongs to the dedicated one-shot presentation family,
            // so the model-condition authority deliberately preserves it.
            // Incremental render extraction still needs an explicit dirty
            // edge or the newly selected model will remain in the ECS only.
            markObjectDirty(
                registry, entity,
                objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        }
    }
}

[[nodiscard]] bool hasToppleUpdate(const ecs::registry& registry,
                                   ecs::entity entity) {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype && type->archetype->tacticalPlan &&
           !type->archetype->tacticalPlan->topple.empty();
}

} // namespace

namespace engine {
void ObjectNeutronMissileSlowDeathSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype
        ? type->archetype->neutronMissileSlowDeathPlan : nullptr;
    if (!plan) return;
    ObjectNeutronMissileSlowDeathComponent value;
    value.plan = plan;
    value.instances.resize(plan->rules.size());
    if (ObjectNeutronMissileSlowDeathComponent* existing =
            ecs::try_get<ObjectNeutronMissileSlowDeathComponent>(registry, entity))
        *existing = std::move(value);
    else ecs::emplace<ObjectNeutronMissileSlowDeathComponent>(
        registry, entity, std::move(value));
}

bool ObjectNeutronMissileSlowDeathSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectNeutronMissilePresentationEvent>& outEvents) const {
    struct Candidate { ObjectId id; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
        ObjectNeutronMissileSlowDeathComponent,
        const ObjectSlowDeathRuntimeComponent,
        const ObjectDeathReactionComponent,
        const TransformComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectId id = view.template get<const ObjectIdentityComponent>(entity).id;
        if (id && lifecycle.entityFromId(id)) candidates.push_back({id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });

    for (const Candidate candidate : candidates) {
        auto& component = ecs::get<ObjectNeutronMissileSlowDeathComponent>(
            registry, candidate.entity);
        const auto& death = ecs::get<const ObjectSlowDeathRuntimeComponent>(
            registry, candidate.entity);
        const auto& reactions = ecs::get<const ObjectDeathReactionComponent>(
            registry, candidate.entity);
        if (!component.plan || !reactions.plan ||
            death.selectedRuleIndex >= reactions.plan->rules.size()) continue;
        const uint32_t authoredOrder =
            reactions.plan->rules[death.selectedRuleIndex].authoredOrder;
        const auto ruleIt = std::lower_bound(
            component.plan->rules.begin(), component.plan->rules.end(), authoredOrder,
            [](const auto& rule, uint32_t order) { return rule.authoredOrder < order; });
        if (ruleIt == component.plan->rules.end() ||
            ruleIt->authoredOrder != authoredOrder) continue;
        const size_t instanceIndex = static_cast<size_t>(
            ruleIt - component.plan->rules.begin());
        if (instanceIndex >= component.instances.size()) continue;
        auto& runtime = component.instances[instanceIndex];
        runtime.lastUpdateTick = confirmedTick;
        const LogicFixedVec3 center = positionOf(
            registry, candidate.entity,
            ecs::get<const TransformComponent>(registry, candidate.entity));
        if (!runtime.activated) {
            runtime.activated = true;
            // NeutronMissileSlowDeathBehavior records m_activationFrame in
            // its first activated Update, not in SlowDeathBehavior::onDie.
            // Keeping this separate matters when death dispatch occurs after
            // this system's update barrier in the same confirmed frame.
            runtime.activationTick = confirmedTick;
            LogicFixedVec3 ground = center;
            ground.z = math::q32_32::from_raw(
                terrain.groundHeightRaw(center.x.raw(), center.y.raw()));
            if (!ruleIt->fxList.empty()) outEvents.push_back({
                .kind = ObjectNeutronMissilePresentationEventKind::InitialFx,
                .source = candidate.id, .fxList = ruleIt->fxList,
                .position = ground, .authoredOrder = authoredOrder,
                .confirmedTick = confirmedTick});
        }

        for (size_t blastIndex = 0; blastIndex < ruleIt->blasts.size(); ++blastIndex) {
            const auto& blast = ruleIt->blasts[blastIndex];
            if (!blast.enabled) continue;
            // Derived from the mask type, whose width the static_assert beside
            // it ties to the authored blast extent, so this shift can never
            // truncate to a zero bit that would re-fire the blast every tick.
            const ObjectNeutronMissileBlastMask bit =
                static_cast<ObjectNeutronMissileBlastMask>(
                    ObjectNeutronMissileBlastMask{1} << blastIndex);
            const uint64_t blastTick = runtime.activationTick +
                firstTickAfterDuration(blast.delayMilliseconds,
                                       rules.logicFramesPerSecond);
            if ((runtime.completedBlastMask & bit) == 0 &&
                confirmedTick >= blastTick) {
                if (blast.outerRadius > Fixed{}) {
                    container::Vector<ObjectId>& targets =
                        runtime.blastTargets[blastIndex];
                    if ((runtime.snapshottedBlastTargetsMask & bit) == 0) {
                        runtime.snapshottedBlastTargetsMask |= bit;
                        targets.clear();
                        runtime.nextBlastTarget[blastIndex] = 0;
                        const auto targetsView = ecs::view<
                            const ObjectIdentityComponent,
                            const TransformComponent>(registry);
                        targets.reserve(targetsView.size_hint());
                        for (const ecs::entity entity : targetsView) {
                            const ObjectId id = targetsView.template get<
                                const ObjectIdentityComponent>(entity).id;
                            if (!id || lifecycle.isPendingDestroy(id) ||
                                !sameMapStatus(
                                    registry, candidate.entity, entity)) {
                                continue;
                            }
                            const LogicFixedVec3 position = positionOf(
                                registry, entity, targetsView.template get<
                                    const TransformComponent>(entity));
                            if (squared2D(center, position) <=
                                blast.outerRadius * blast.outerRadius) {
                                targets.push_back(id);
                            }
                        }
                        std::sort(targets.begin(), targets.end());
                    }

                    while (runtime.nextBlastTarget[blastIndex] <
                           targets.size()) {
                        const ObjectId targetId = targets[
                            runtime.nextBlastTarget[blastIndex]++];
                        const std::optional<ecs::entity> targetEntity =
                            lifecycle.entityFromId(targetId);
                        if (!targetEntity ||
                            !sameMapStatus(
                                registry, candidate.entity, *targetEntity)) {
                            continue;
                        }
                        const TransformComponent* targetTransform =
                            ecs::try_get<TransformComponent>(
                                registry, *targetEntity);
                        if (!targetTransform) continue;
                        const LogicFixedVec3 position = positionOf(
                            registry, *targetEntity, *targetTransform);
                        if (squared2D(center, position) >
                            blast.outerRadius * blast.outerRadius) {
                            continue;
                        }
                        const LogicFixedVec3 force{
                            position.x - center.x, position.y - center.y,
                            position.z - center.z};
                        if (hasToppleUpdate(registry, *targetEntity)) {
                            queueObjectToppleRequest(registry, {
                                .object = targetId,
                                .source = candidate.id,
                                .direction = force,
                                .speed = blast.toppleSpeed,
                                .sourceSequence = static_cast<uint32_t>(
                                    blastIndex + 1u),
                                .confirmedTick = confirmedTick,
                                .noBounce = true,
                                .noFx = true,
                            });
                        }
                        const Fixed distance = Fixed::sqrt(
                            force.x * force.x + force.y * force.y + force.z * force.z);
                        Fixed damage = blast.maximumDamage;
                        if (distance > blast.innerRadius) {
                            const Fixed denominator = blast.outerRadius -
                                blast.innerRadius + Fixed::from_fraction(1, 100);
                            damage = blast.maximumDamage *
                                (Fixed{int32_t{1}} -
                                 (distance - blast.innerRadius) / denominator);
                            damage = std::max(damage, blast.minimumDamage);
                        }
                        if (damage > Fixed{}) outDamage.push_back({
                            .target = targetId, .source = candidate.id,
                            .sourceSequence = static_cast<uint32_t>(blastIndex + 1),
                            .amount = damage,
                            .damageType = game::DamageType::EXPLOSION,
                            .deathType = game::DeathType::EXPLODED,
                            .confirmedTick = confirmedTick});
                        if (damage > Fixed{} && !runtime.scorchPlaced) {
                            runtime.scorchPlaced = true;
                            outEvents.push_back({
                                .kind = ObjectNeutronMissilePresentationEventKind::ScorchMark,
                                .source = candidate.id, .position = center,
                                .size = ruleIt->scorchMarkSize,
                                .authoredOrder = authoredOrder,
                                .confirmedTick = confirmedTick});
                        }
                        // Return after exactly one target. Session commits the
                        // paired Topple + Damage transaction before this Blast
                        // searches for the next stable ObjectId.
                        return true;
                    }
                }
                runtime.completedBlastMask |= bit;
                runtime.blastTargets[blastIndex].clear();
                // Closing an empty/finished Blast is itself a causal step; the
                // next invocation may now advance a simultaneously due Blast
                // against the fully committed world.
                return true;
            }
            const uint64_t scorchTick = runtime.activationTick +
                firstTickAfterDuration(blast.scorchDelayMilliseconds,
                                       rules.logicFramesPerSecond);
            if ((runtime.completedScorchMask & bit) == 0 &&
                confirmedTick >= scorchTick) {
                runtime.completedScorchMask |= bit;
                if (blast.outerRadius <= Fixed{}) continue;
                const auto targets = ecs::view<const ObjectIdentityComponent,
                                               const TransformComponent>(registry);
                for (const ecs::entity entity : targets) {
                    const ObjectId id = targets.template get<
                        const ObjectIdentityComponent>(entity).id;
                    if (!id || lifecycle.isPendingDestroy(id) ||
                        !sameMapStatus(registry, candidate.entity, entity)) continue;
                    const LogicFixedVec3 position = positionOf(
                        registry, entity, targets.template get<
                            const TransformComponent>(entity));
                    if (squared2D(center, position) >
                        blast.outerRadius * blast.outerRadius) continue;
                    setBurned(registry, entity, confirmedTick);
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(registry, entity);
                    if (kinds && game::objectHasKind(
                            kinds->mask, game::ObjectKindOf::Shrubbery)) {
                        ObjectShadowSuppressionComponent suppression{
                            confirmedTick};
                        if (auto* existing = ecs::try_get<
                                ObjectShadowSuppressionComponent>(registry,
                                                                  entity))
                            *existing = suppression;
                        else {
                            ecs::emplace<ObjectShadowSuppressionComponent>(
                                registry, entity, suppression);
                            // The shadow flag is extracted from this sparse
                            // component; without this edge a cached render
                            // entity keeps its pre-blast shadow indefinitely.
                            markObjectDirty(
                                registry, entity,
                                ObjectDirtyDomain::RenderExtraction);
                        }
                    }
                }
            }
        }
    }
    return false;
}
} // namespace engine

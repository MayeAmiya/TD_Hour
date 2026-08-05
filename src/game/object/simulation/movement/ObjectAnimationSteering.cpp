#include "game/object/simulation/movement/ObjectAnimationSteering.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace engine {
namespace {

enum class TurningDirection : int8_t {
    Right = -1,
    None = 0,
    Left = 1,
};

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

struct AnimationSteeringActiveSetCache final {
    container::Vector<ObjectId> ids;
    bool initialized = false;
};

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] math::q32_32 signedAngleDelta(
    math::q32_32 current, math::q32_32 previous) noexcept {
    constexpr math::q32_32 kTwoPi =
        math::q32_32::from_raw(26986075409ll);
    int64_t delta = (current - previous).raw() % kTwoPi.raw();
    constexpr math::q32_32 kPi =
        math::q32_32::from_raw(13493037705ll);
    if (delta > kPi.raw()) delta -= kTwoPi.raw();
    if (delta < -kPi.raw()) delta += kTwoPi.raw();
    return math::q32_32::from_raw(delta);
}

[[nodiscard]] TurningDirection currentTurning(
    const ecs::registry& registry, ecs::entity entity,
    math::q32_32 rotationDelta) noexcept {
    const math::q32_32 kTurningEpsilon =
        math::q32_32::from_fraction(1, 100000);
    if (rotationDelta < -kTurningEpsilon) return TurningDirection::Right;
    if (rotationDelta > kTurningEpsilon) return TurningDirection::Left;

    const ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    if (!physics) return TurningDirection::None;
    const math::q32_32 yawRate = physics->yawRate;
    if (yawRate < -kTurningEpsilon) return TurningDirection::Right;
    if (yawRate > kTurningEpsilon) return TurningDirection::Left;
    return TurningDirection::None;
}

[[nodiscard]] const game::ModelConditionMask& steeringConditions() {
    static const game::ModelConditionMask conditions =
        game::modelConditionMaskOf(game::ModelConditionFlag::CenterToRight, game::ModelConditionFlag::CenterToLeft, game::ModelConditionFlag::RightToCenter, game::ModelConditionFlag::LeftToCenter);
    return conditions;
}

[[nodiscard]] const game::ModelConditionMask& conditionFor(
    ObjectSteeringAnimationPhase phase) {
    static const game::ModelConditionMask empty{};
    static const game::ModelConditionMask centerToRight =
        game::modelConditionMaskOf(game::ModelConditionFlag::CenterToRight);
    static const game::ModelConditionMask centerToLeft =
        game::modelConditionMaskOf(game::ModelConditionFlag::CenterToLeft);
    static const game::ModelConditionMask rightToCenter =
        game::modelConditionMaskOf(game::ModelConditionFlag::RightToCenter);
    static const game::ModelConditionMask leftToCenter =
        game::modelConditionMaskOf(game::ModelConditionFlag::LeftToCenter);
    switch (phase) {
    case ObjectSteeringAnimationPhase::CenterToRight: return centerToRight;
    case ObjectSteeringAnimationPhase::CenterToLeft: return centerToLeft;
    case ObjectSteeringAnimationPhase::RightToCenter: return rightToCenter;
    case ObjectSteeringAnimationPhase::LeftToCenter: return leftToCenter;
    case ObjectSteeringAnimationPhase::Centered: return empty;
    }
    return empty;
}

void publishPhase(RenderModelComponent& visual,
                  ObjectSteeringAnimationPhase phase) noexcept {
    visual.modelConditionFlags.clear(steeringConditions());
    const game::ModelConditionMask& selected = conditionFor(phase);
    for (size_t index = 0; index < selected.words.size(); ++index) {
        visual.modelConditionFlags.words[index] |= selected.words[index];
    }
}

} // namespace

void ObjectAnimationSteeringSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype ||
        !type->archetype->animationSteeringPlan) {
        return;
    }
    ObjectAnimationSteeringComponent value{
        .plan = type->archetype->animationSteeringPlan,
    };
    value.instances.resize(value.plan->rules.size());
    if (const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
        transform && transform->authoritative) {
        value.previousRotation = transform->yawRadians;
        value.rotationInitialized = true;
    }
    if (ObjectAnimationSteeringComponent* existing =
            ecs::try_get<ObjectAnimationSteeringComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectAnimationSteeringComponent>(registry, entity,
                                                       std::move(value));
    }
}

void ObjectAnimationSteeringSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    AnimationSteeringActiveSetCache* activeSet =
        registry.ctx().find<AnimationSteeringActiveSetCache>();
    if (!activeSet) {
        activeSet =
            &registry.ctx().emplace<AnimationSteeringActiveSetCache>();
    }
    container::Vector<ObjectId> wakeIds;
    if (!activeSet->initialized) {
        const auto bootstrap = ecs::view<
            const ObjectIdentityComponent,
            ObjectAnimationSteeringComponent>(registry);
        wakeIds.reserve(bootstrap.size_hint());
        for (const ecs::entity entity : bootstrap) {
            const ObjectId id = bootstrap
                .template get<const ObjectIdentityComponent>(entity).id;
            if (id) wakeIds.push_back(id);
        }
        activeSet->initialized = true;
    } else {
        const auto dirtyView = ecs::view<
            const ObjectIdentityComponent,
            ObjectAnimationSteeringComponent,
            const ObjectDirtyComponent>(registry);
        wakeIds.reserve(dirtyView.size_hint());
        for (const ecs::entity entity : dirtyView) {
            const ObjectDirtyComponent& dirty = dirtyView
                .template get<const ObjectDirtyComponent>(entity);
            if ((dirty.domains & objectDirtyBit(
                    ObjectDirtyDomain::RenderExtraction)) == 0) {
                continue;
            }
            const ObjectId id = dirtyView
                .template get<const ObjectIdentityComponent>(entity).id;
            if (id) wakeIds.push_back(id);
        }
    }
    activeSet->ids.insert(activeSet->ids.end(), wakeIds.begin(),
                          wakeIds.end());
    std::sort(activeSet->ids.begin(), activeSet->ids.end());
    activeSet->ids.erase(
        std::unique(activeSet->ids.begin(), activeSet->ids.end()),
        activeSet->ids.end());

    container::Vector<Candidate> candidates;
    candidates.reserve(activeSet->ids.size());
    for (const ObjectId id : activeSet->ids) {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(id);
        if (!entity || lifecycle.isPendingDestroy(id) ||
            !ecs::try_get<ObjectAnimationSteeringComponent>(registry,
                                                             *entity) ||
            !ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                          *entity) ||
            !ecs::try_get<RenderModelComponent>(registry, *entity) ||
            !ecs::try_get<ObjectPhysicsComponent>(registry, *entity)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, *entity);
        if ((health && health->effectivelyDead) || (map && map->offMap))
            continue;
        candidates.push_back({id, *entity});
    }

    container::Vector<ObjectId> survivors;
    survivors.reserve(candidates.size());

    for (const Candidate& candidate : candidates) {
        ObjectAnimationSteeringComponent& component =
            ecs::get<ObjectAnimationSteeringComponent>(registry,
                                                        candidate.entity);
        const ObjectFixedTransformComponent& transform =
            ecs::get<const ObjectFixedTransformComponent>(
                registry, candidate.entity);
        if (!transform.authoritative) continue;
        if (component.rotationInitialized &&
            component.previousRotation == transform.yawRadians &&
            confirmedTick < component.nextDueTick) {
            survivors.push_back(candidate.object);
            continue;
        }
        if (!component.rotationInitialized) {
            component.previousRotation = transform.yawRadians;
            component.rotationInitialized = true;
        }
        const math::q32_32 delta = signedAngleDelta(
            transform.yawRadians, component.previousRotation);
        component.previousRotation = transform.yawRadians;
        const TurningDirection turning =
            currentTurning(registry, candidate.entity, delta);
        RenderModelComponent& visual =
            ecs::get<RenderModelComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }

        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectAnimationSteeringRuntime& runtime =
                component.instances[index];
            if (confirmedTick < runtime.nextTransitionTick) continue;
            const uint64_t transitionFrames = millisecondsToFrames(
                component.plan->rules[index].minimumTransitionMilliseconds,
                rules.logicFramesPerSecond);
            const auto transitionTo = [&](ObjectSteeringAnimationPhase phase) {
                runtime.phase = phase;
                runtime.nextTransitionTick =
                    saturatingAdd(confirmedTick, transitionFrames);
                publishPhase(visual, phase);
                markObjectDirty(registry, candidate.entity,
                                ObjectDirtyDomain::RenderExtraction);
            };

            switch (runtime.phase) {
            case ObjectSteeringAnimationPhase::Centered:
                if (turning == TurningDirection::Right) {
                    transitionTo(ObjectSteeringAnimationPhase::CenterToRight);
                } else if (turning == TurningDirection::Left) {
                    transitionTo(ObjectSteeringAnimationPhase::CenterToLeft);
                }
                break;
            case ObjectSteeringAnimationPhase::CenterToRight:
                if (turning != TurningDirection::Right) {
                    transitionTo(ObjectSteeringAnimationPhase::RightToCenter);
                }
                break;
            case ObjectSteeringAnimationPhase::CenterToLeft:
                if (turning != TurningDirection::Left) {
                    transitionTo(ObjectSteeringAnimationPhase::LeftToCenter);
                }
                break;
            case ObjectSteeringAnimationPhase::RightToCenter:
            case ObjectSteeringAnimationPhase::LeftToCenter:
                if (turning == TurningDirection::None) {
                    transitionTo(ObjectSteeringAnimationPhase::Centered);
                }
                break;
            }
        }

        component.nextDueTick = std::numeric_limits<uint64_t>::max();
        bool remainsActive = turning != TurningDirection::None;
        for (const ObjectAnimationSteeringRuntime& runtime :
             component.instances) {
            if (runtime.phase == ObjectSteeringAnimationPhase::Centered)
                continue;
            remainsActive = true;
            component.nextDueTick = std::min(
                component.nextDueTick, runtime.nextTransitionTick);
        }
        if (remainsActive) survivors.push_back(candidate.object);
    }
    activeSet->ids = std::move(survivors);
}

} // namespace engine

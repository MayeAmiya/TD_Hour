#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) * rate;
    return (numerator + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(
    uint64_t value, uint64_t increment) noexcept {
    return increment > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max()
        : value + increment;
}

[[nodiscard]] const ObjectSpecialPowerRuntime* findSpecialPowerRuntime(
    const ObjectSpecialPowerComponent& powers,
    SpecialPowerContentId content) noexcept {
    const auto found = std::find_if(
        powers.instances.begin(), powers.instances.end(),
        [content](const ObjectSpecialPowerRuntime& runtime) {
            return runtime.content == content;
        });
    return found == powers.instances.end() ? nullptr : &*found;
}

void advanceFxSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

void advanceConditionAnimationRevision(uint64_t& revision) noexcept {
    ++revision;
    if (revision == 0) ++revision;
}

[[nodiscard]] const container::String& fxForPhase(
    const game::ObjectMissileLauncherBuildingRule& rule,
    ObjectMissileLauncherDoorPhase phase) noexcept {
    switch (phase) {
    case ObjectMissileLauncherDoorPhase::Closed: return rule.doorClosedFx;
    case ObjectMissileLauncherDoorPhase::Opening: return rule.doorOpeningFx;
    case ObjectMissileLauncherDoorPhase::Open: return rule.doorOpenFx;
    case ObjectMissileLauncherDoorPhase::WaitingToClose:
        return rule.doorWaitingToCloseFx;
    case ObjectMissileLauncherDoorPhase::Closing: return rule.doorClosingFx;
    }
    return rule.doorClosedFx;
}

void switchPhase(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    const game::ObjectMissileLauncherBuildingRule& rule,
    ObjectMissileLauncherBuildingRuntime& runtime,
    ObjectMissileLauncherDoorPhase destination,
    const ObjectSpecialPowerRuntime& power, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond,
    uint64_t& nextFxEmissionSequence,
    container::Vector<ObjectMissileLauncherFxEvent>& outFx,
    container::Vector<ObjectFireAudioCommand>& outAudio) {
    if (runtime.phase == destination) return;

    runtime.phase = destination;
    runtime.stateEnteredTick = confirmedTick;
    runtime.visibleDurationTicks = 0;
    switch (destination) {
    case ObjectMissileLauncherDoorPhase::Closed:
        runtime.timeoutTick = 0;
        runtime.timeoutPhase = ObjectMissileLauncherDoorPhase::Closed;
        break;
    case ObjectMissileLauncherDoorPhase::Opening:
        runtime.timeoutTick = power.readyTick == 0 ? 0 : power.readyTick - 1u;
        runtime.timeoutPhase = ObjectMissileLauncherDoorPhase::Open;
        break;
    case ObjectMissileLauncherDoorPhase::Open:
        runtime.timeoutTick = 0;
        runtime.timeoutPhase = ObjectMissileLauncherDoorPhase::Open;
        break;
    case ObjectMissileLauncherDoorPhase::WaitingToClose:
        runtime.timeoutTick = saturatingAdd(
            confirmedTick, runtime.doorWaitOpenTicks);
        runtime.timeoutPhase = ObjectMissileLauncherDoorPhase::Closing;
        break;
    case ObjectMissileLauncherDoorPhase::Closing: {
        const uint64_t authoredDeadline = saturatingAdd(
            confirmedTick, runtime.doorCloseTicks);
        const uint64_t remaining = power.readyTick > confirmedTick
            ? power.readyTick - confirmedTick : 0;
        runtime.timeoutTick = std::min(
            authoredDeadline,
            saturatingAdd(confirmedTick, remaining / 2u));
        runtime.timeoutPhase = ObjectMissileLauncherDoorPhase::Closed;
        break;
    }
    }
    if (runtime.timeoutTick > confirmedTick) {
        runtime.visibleDurationTicks =
            runtime.timeoutTick - confirmedTick;
    }
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        // RefCode applies clearAndSetModelConditionFlags() before
        // setAnimationLoopDuration(). Publish the equivalent as one authored
        // presentation transaction. The revision identifies the paired phase
        // and duration mutation; ordinary ConditionState selection itself is
        // immediate, while stale renderer completions remain generation
        // checked at the presentation boundary.
        advanceConditionAnimationRevision(
            visual->conditionAnimationRevision);
        visual->animationLoopDurationSeconds =
            runtime.visibleDurationTicks == 0
            ? 0.0f
            : static_cast<float>(runtime.visibleDurationTicks) /
                  static_cast<float>(std::max<uint32_t>(
                  1, logicFramesPerSecond));
    }

    // The model-condition authority derives DOOR_1_* from this runtime.  The
    // phase transition is therefore a render-affecting mutation even when no
    // transform changed; without the dirty marks the renderer keeps the old
    // closed/open pose until some unrelated object change refreshes it.
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));

    if (runtime.openIdleAudioActive &&
        destination != ObjectMissileLauncherDoorPhase::Open) {
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StopLoop,
            .object = object,
            .eventName = rule.doorOpenIdleAudio,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        runtime.openIdleAudioActive = false;
    }
    if (destination == ObjectMissileLauncherDoorPhase::Open &&
        !runtime.openIdleAudioActive && !rule.doorOpenIdleAudio.empty()) {
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StartLoop,
            .object = object,
            .eventName = rule.doorOpenIdleAudio,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        runtime.openIdleAudioActive = true;
    }

    const container::String& fx = fxForPhase(rule, destination);
    if (!fx.empty()) {
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity);
        if (transform) {
            outFx.push_back({
                .object = object,
                .phase = destination,
                .fxList = fx,
                .position = readAuthoritativeObjectPosition(
                    registry, entity, *transform),
                .animationDurationTicks = runtime.visibleDurationTicks,
                .authoredOrder = rule.authoredOrder,
                .emissionSequence = nextFxEmissionSequence,
                .confirmedTick = confirmedTick,
            });
            advanceFxSequence(nextFxEmissionSequence);
        }
    }
}

} // namespace

void notifyMissileLauncherSpecialPowerActivated(
    ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower, uint64_t confirmedTick) noexcept {
    ObjectMissileLauncherBuildingComponent* component =
        ecs::try_get<ObjectMissileLauncherBuildingComponent>(registry, entity);
    if (!component || !specialPower) return;
    for (ObjectMissileLauncherBuildingRuntime& runtime :
         component->instances) {
        if (runtime.specialPower != specialPower) continue;
        runtime.activationRequested = true;
        runtime.activationRequestTick = confirmedTick;
    }
}

bool missileLauncherActivationMustWaitForOpenDoor(
    const ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower) noexcept {
    const ObjectMissileLauncherBuildingComponent* component =
        ecs::try_get<ObjectMissileLauncherBuildingComponent>(registry, entity);
    if (!component || !specialPower) return false;
    for (const ObjectMissileLauncherBuildingRuntime& runtime :
         component->instances) {
        if (runtime.specialPower != specialPower) continue;
        return runtime.phase != ObjectMissileLauncherDoorPhase::Open;
    }
    return false;
}

void ObjectMissileLauncherBuildingSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype
        ? type->archetype->missileLauncherBuildingPlan : nullptr;
    if (!plan || plan->rules.empty()) {
        ecs::remove<ObjectMissileLauncherBuildingComponent>(registry, entity);
        return;
    }
    ObjectMissileLauncherBuildingComponent component{.plan = plan};
    component.instances.reserve(plan->rules.size());
    for (const game::ObjectMissileLauncherBuildingRule& rule : plan->rules) {
        ObjectMissileLauncherBuildingRuntime runtime;
        if (const SpecialPowerDefinition* definition =
                content.findSpecialPower(rule.specialPowerTemplate)) {
            runtime.specialPower = definition->id;
        }
        runtime.doorOpenTicks = millisecondsToTicks(
            rule.doorOpenMilliseconds, logicFramesPerSecond);
        runtime.doorWaitOpenTicks = millisecondsToTicks(
            rule.doorWaitOpenMilliseconds, logicFramesPerSecond);
        runtime.doorCloseTicks = millisecondsToTicks(
            rule.doorCloseMilliseconds, logicFramesPerSecond);
        runtime.stateEnteredTick = confirmedTick;
        component.instances.push_back(runtime);
    }
    if (ObjectMissileLauncherBuildingComponent* existing =
            ecs::try_get<ObjectMissileLauncherBuildingComponent>(
                registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectMissileLauncherBuildingComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectMissileLauncherBuildingSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, uint64_t& nextFxEmissionSequence,
    container::Vector<ObjectMissileLauncherFxEvent>& outFx,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    static_cast<void>(content);
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<
        const ObjectIdentityComponent,
        ObjectMissileLauncherBuildingComponent,
        const ObjectSpecialPowerComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && lifecycle.entityFromId(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        if (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) {
            continue;
        }
        ObjectMissileLauncherBuildingComponent& component =
            ecs::get<ObjectMissileLauncherBuildingComponent>(
                registry, candidate.entity);
        const ObjectSpecialPowerComponent& powers =
            ecs::get<const ObjectSpecialPowerComponent>(
                registry, candidate.entity);
        const size_t count = std::min(
            component.plan ? component.plan->rules.size() : 0u,
            component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectMissileLauncherBuildingRule& rule =
                component.plan->rules[index];
            ObjectMissileLauncherBuildingRuntime& runtime =
                component.instances[index];
            const ObjectSpecialPowerRuntime* power =
                findSpecialPowerRuntime(powers, runtime.specialPower);
            if (!power) continue;

            if (runtime.activationRequested &&
                runtime.activationRequestTick <= confirmedTick) {
                runtime.activationRequested = false;
                switchPhase(
                    registry, candidate.entity, candidate.object, rule,
                    runtime,
                    ObjectMissileLauncherDoorPhase::WaitingToClose,
                    *power, confirmedTick, logicFramesPerSecond,
                    nextFxEmissionSequence,
                    outFx, outAudio);
            }
            if (runtime.timeoutTick != 0 &&
                confirmedTick > runtime.timeoutTick) {
                switchPhase(
                    registry, candidate.entity, candidate.object, rule,
                    runtime, runtime.timeoutPhase, *power, confirmedTick,
                    logicFramesPerSecond,
                    nextFxEmissionSequence, outFx, outAudio);
            }

            const uint64_t whenToStartOpening =
                power->readyTick >= runtime.doorOpenTicks
                ? power->readyTick - runtime.doorOpenTicks : 0;
            const bool mayAutomaticallyReachOpen =
                runtime.phase == ObjectMissileLauncherDoorPhase::Closed ||
                runtime.phase == ObjectMissileLauncherDoorPhase::Opening;
            if (mayAutomaticallyReachOpen &&
                confirmedTick >= power->readyTick) {
                switchPhase(
                    registry, candidate.entity, candidate.object, rule,
                    runtime, ObjectMissileLauncherDoorPhase::Open, *power,
                    confirmedTick, logicFramesPerSecond,
                    nextFxEmissionSequence, outFx, outAudio);
            } else if (runtime.phase ==
                           ObjectMissileLauncherDoorPhase::Closed &&
                       confirmedTick >= whenToStartOpening) {
                switchPhase(
                    registry, candidate.entity, candidate.object, rule,
                    runtime, ObjectMissileLauncherDoorPhase::Opening,
                    *power, confirmedTick, logicFramesPerSecond,
                    nextFxEmissionSequence,
                    outFx, outAudio);
            }
        }
    }
}

void ObjectMissileLauncherBuildingSystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectFireAudioCommand>& outAudio) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectMissileLauncherBuildingComponent* component = entity
        ? ecs::try_get<ObjectMissileLauncherBuildingComponent>(
              registry, *entity)
        : nullptr;
    if (!component || !component->plan) return;
    const size_t count = std::min(
        component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectMissileLauncherBuildingRuntime& runtime =
            component->instances[index];
        const game::ObjectMissileLauncherBuildingRule& rule =
            component->plan->rules[index];
        if (!runtime.openIdleAudioActive) continue;
        outAudio.push_back({
            .kind = ObjectFireAudioCommandKind::StopLoop,
            .object = object,
            .eventName = rule.doorOpenIdleAudio,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = confirmedTick,
        });
        runtime.openIdleAudioActive = false;
    }
}

} // namespace engine

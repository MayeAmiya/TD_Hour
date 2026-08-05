#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/lifecycle/ObjectDeathWalk.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace engine
{
namespace
{

[[nodiscard]] uint64_t millisecondsToFrames(uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept
{
    if (milliseconds == 0)
        return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    if (left > std::numeric_limits<uint64_t>::max() - right)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] bool aliveForUpdate(const ecs::registry& registry,
                                  const ObjectLifecycle& lifecycle,
                                  ObjectId object,
                                  ecs::entity entity) noexcept
{
    if (!object || lifecycle.isPendingDestroy(object))
        return false;
    const ObjectLifecycleComponent* life = ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    if (life && life->phase != ObjectLifecyclePhase::Alive)
        return false;
    const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health && health->effectivelyDead)
        return false;
    const ObjectMapStatusComponent* map = ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return !map || !map->offMap;
}

[[nodiscard]] bool ownerIsPlayable(const PlayerRegistry& players, PlayerId owner) noexcept
{
    const PlayerState* player = players.get(owner);
    return player && player->isPlayableSide();
}

void setCapturedCondition(RenderModelComponent* visual, bool captured)
{
    if (!visual)
        return;
    static const game::ModelConditionMask capturedMask = game::modelConditionMaskOf(game::ModelConditionFlag::Captured);
    if (!captured)
    {
        visual->modelConditionFlags.clear(capturedMask);
        return;
    }
    for (size_t index = 0; index < capturedMask.words.size(); ++index)
    {
        visual->modelConditionFlags.words[index] |= capturedMask.words[index];
    }
}

[[nodiscard]] LogicFixedVec3 snapshotPosition(const ecs::registry& registry, ecs::entity entity) noexcept
{
    const TransformComponent* transform = ecs::try_get<TransformComponent>(registry, entity);
    return transform
        ? readAuthoritativeObjectPosition(registry, entity, *transform)
        : LogicFixedVec3{};
}

void refreshTechBuilding(ecs::registry& registry,
                         ecs::entity entity,
                         ObjectId object,
                         const PlayerRegistry& players,
                         const ObjectSimulationRules& rules,
                         uint64_t confirmedTick,
                         ObjectTechBuildingComponent& component,
                         container::Vector<ObjectTechBuildingEvent>& outEvents)
{
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    const bool captured = owner && ownerIsPlayable(players, owner->player);
    setCapturedCondition(ecs::try_get<RenderModelComponent>(registry, entity), captured);
    for (size_t index = 0; index < component.techBuildings.size(); ++index)
    {
        ObjectTechBuildingRuntime& runtime = component.techBuildings[index];
        const game::ObjectTechBuildingRule& rule = component.plan->techBuildings[index];
        if (runtime.captured != captured)
        {
            runtime.captured = captured;
            runtime.nextPulseTick = confirmedTick;
            outEvents.push_back({
                .kind = ObjectTechBuildingEventKind::CapturedStateChanged,
                .object = object,
                .owner = owner ? owner->player : INVALID_PLAYER_ID,
                .authoredOrder = rule.authoredOrder,
                .captured = captured,
                .confirmedTick = confirmedTick,
            });
        }
        if (!captured || rule.pulseFx.empty() || rule.pulseFxRateMilliseconds == 0)
        {
            continue;
        }
        if (confirmedTick < runtime.nextPulseTick)
            continue;
        outEvents.push_back({
            .kind = ObjectTechBuildingEventKind::PulseFx,
            .object = object,
            .owner = owner ? owner->player : INVALID_PLAYER_ID,
            .authoredOrder = rule.authoredOrder,
            .fxList = rule.pulseFx,
            .captured = true,
            .confirmedTick = confirmedTick,
        });
        runtime.nextPulseTick = saturatingAdd(
            confirmedTick,
            std::max<uint64_t>(1u, millisecondsToFrames(rule.pulseFxRateMilliseconds, rules.logicFramesPerSecond)));
    }
}

void emitBeaconState(const ecs::registry& registry,
                     ecs::entity entity,
                     ObjectId object,
                     const game::ObjectBeaconClientRule& rule,
                     ObjectBeaconClientEventKind kind,
                     uint32_t durationMilliseconds,
                     uint64_t confirmedTick,
                     container::Vector<ObjectBeaconClientEvent>& outEvents)
{
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    outEvents.push_back({
        .kind = kind,
        .object = object,
        .owner = owner ? owner->player : INVALID_PLAYER_ID,
        .authoredOrder = rule.authoredOrder,
        // PlayerId is the authoritative color key. The presentation adapter
        // resolves it through the frozen MultiplayerRuleset (and may apply a
        // script custom-indicator override); inventing RGB from the numeric
        // PlayerId would make beacon colors content-incompatible.
        .indicatorColorRgb = 0xffffffu,
        .radarPulseDurationMilliseconds = durationMilliseconds,
        .position = snapshotPosition(registry, entity),
        .confirmedTick = confirmedTick,
    });
}

} // namespace

void ObjectTechBuildingSystem::initializeObject(ecs::registry& registry,
                                                ecs::entity entity,
                                                const ObjectSimulationRules& rules) const
{
    const ThingTemplateComponent* type = ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectTechBuildingPlan> plan =
        type && type->archetype ? type->archetype->techBuildingPlan : nullptr;
    if (!plan)
        return;

    const ObjectLifecycleComponent* lifecycle = ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const uint64_t createdAtTick = lifecycle ? lifecycle->createdAtTick : 0u;
    ObjectTechBuildingComponent value{
        .plan = plan,
    };
    value.techBuildings.resize(plan->techBuildings.size());
    value.beacons.resize(plan->beacons.size());
    for (size_t index = 0; index < value.beacons.size(); ++index)
    {
        ObjectBeaconClientRuntime& runtime = value.beacons[index];
        const game::ObjectBeaconClientRule& rule = plan->beacons[index];
        const uint64_t frequency = millisecondsToFrames(
            rule.radarPulseFrequencyMilliseconds,
            rules.logicFramesPerSecond);
        runtime.nextRadarPulseTick = saturatingAdd(
            saturatingAdd(createdAtTick, frequency), 1u);
    }
    if (ObjectTechBuildingComponent* existing = ecs::try_get<ObjectTechBuildingComponent>(registry, entity))
    {
        *existing = std::move(value);
    }
    else
    {
        ecs::emplace<ObjectTechBuildingComponent>(registry, entity, std::move(value));
    }
}

void ObjectTechBuildingSystem::onObjectOwnerChanged(ecs::registry& registry,
                                                    const ObjectLifecycle& lifecycle,
                                                    const PlayerRegistry& players,
                                                    ObjectId object,
                                                    const ObjectSimulationRules& rules,
                                                    uint64_t confirmedTick,
                                                    container::Vector<ObjectTechBuildingEvent>& outTechEvents,
                                                    container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return;
    ObjectTechBuildingComponent* component = ecs::try_get<ObjectTechBuildingComponent>(registry, *entity);
    if (!component || !component->plan)
        return;
    refreshTechBuilding(registry, *entity, object, players, rules, confirmedTick, *component, outTechEvents);
    for (size_t index = 0; index < component->beacons.size(); ++index)
    {
        emitBeaconState(registry,
                        *entity,
                        object,
                        component->plan->beacons[index],
                        ObjectBeaconClientEventKind::ShowSmoke,
                        0,
                        confirmedTick,
                        outBeaconEvents);
    }
}

void ObjectTechBuildingSystem::onObjectReclaim(
    ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object,
    uint64_t confirmedTick,
    container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromIdIncludingPending(object);
    if (!entity)
        return;
    ObjectTechBuildingComponent* component = ecs::try_get<ObjectTechBuildingComponent>(registry, *entity);
    if (!component || !component->plan)
        return;
    setCapturedCondition(ecs::try_get<RenderModelComponent>(registry, *entity), false);
    for (ObjectTechBuildingRuntime& runtime : component->techBuildings)
        runtime.captured = false;
    for (size_t index = 0; index < component->beacons.size(); ++index)
    {
        component->beacons[index].smokeStarted = false;
        emitBeaconState(registry,
                        *entity,
                        object,
                        component->plan->beacons[index],
                        ObjectBeaconClientEventKind::Hide,
                        0,
                        confirmedTick,
                        outBeaconEvents);
    }
}

bool ObjectTechBuildingSystem::onDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t authoredOrder, uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectOwnershipChangeRequest>& ownership) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectTechBuildingComponent* component = entity
        ? ecs::try_get<ObjectTechBuildingComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan) return false;
    for (size_t index = 0;
         index < component->plan->techBuildings.size() &&
         index < component->techBuildings.size(); ++index) {
        if (component->plan->techBuildings[index].authoredOrder !=
            authoredOrder) {
            continue;
        }
        ObjectTechBuildingRuntime& runtime = component->techBuildings[index];
        if (runtime.deathReleased) return true;
        runtime.captured = false;
        runtime.deathReleased = true;
        setCapturedCondition(
            ecs::try_get<RenderModelComponent>(registry, *entity), false);
        ownership.push_back({
            .object = object,
            .owner = NEUTRAL_PLAYER_ID,
            .authoredOrder = authoredOrder,
            .submissionOrdinal = nextGameplaySubmissionOrdinal++,
            .confirmedTick = confirmedTick,
        });
        if (nextGameplaySubmissionOrdinal == 0) {
            ++nextGameplaySubmissionOrdinal;
        }
        return true;
    }
    return false;
}

void ObjectTechBuildingSystem::update(ecs::registry& registry,
                                      const ObjectLifecycle& lifecycle,
                                      const PlayerRegistry& players,
                                      const ObjectSimulationRules& rules,
                                      uint64_t confirmedTick,
                                      container::Vector<ObjectTechBuildingEvent>& outTechEvents,
                                      container::Vector<ObjectBeaconClientEvent>& outBeaconEvents) const
{
    struct Candidate final
    {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectTechBuildingComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view)
    {
        const ObjectIdentityComponent& identity = view.template get<const ObjectIdentityComponent>(entity);
        if (!aliveForUpdate(registry, lifecycle, identity.id, entity))
        {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.object < right.object; });

    for (const Candidate& candidate : candidates)
    {
        ObjectTechBuildingComponent& component = ecs::get<ObjectTechBuildingComponent>(registry, candidate.entity);
        if (!component.plan)
            continue;
        refreshTechBuilding(
            registry, candidate.entity, candidate.object, players, rules, confirmedTick, component, outTechEvents);
        for (size_t index = 0; index < component.beacons.size(); ++index)
        {
            ObjectBeaconClientRuntime& runtime = component.beacons[index];
            const game::ObjectBeaconClientRule& rule = component.plan->beacons[index];
            if (!runtime.smokeStarted)
            {
                runtime.smokeStarted = true;
                emitBeaconState(registry,
                                candidate.entity,
                                candidate.object,
                                rule,
                                ObjectBeaconClientEventKind::ShowSmoke,
                                0,
                                confirmedTick,
                                outBeaconEvents);
            }
            if (confirmedTick < runtime.nextRadarPulseTick)
                continue;
            emitBeaconState(registry,
                            candidate.entity,
                            candidate.object,
                            rule,
                            ObjectBeaconClientEventKind::RadarPulse,
                            rule.radarPulseDurationMilliseconds,
                            confirmedTick,
                            outBeaconEvents);
            runtime.nextRadarPulseTick = saturatingAdd(
                saturatingAdd(
                    confirmedTick,
                    millisecondsToFrames(
                        rule.radarPulseFrequencyMilliseconds,
                        rules.logicFramesPerSecond)),
                1u);
        }
    }
}

} // namespace engine

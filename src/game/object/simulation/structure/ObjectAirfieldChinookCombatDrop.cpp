#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
#include "core/container/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/base/SimulationRandom.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
using namespace airfield_detail;

bool ObjectAirfieldSystem::beginChinookCombatDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, SimulationRandom& random,
    const ObjectChinookCombatDropBeginRequest& request,
    container::Vector<ObjectChinookRopePresentationEvent>& outEvents) const
{
    if (!objectAlive(registry, lifecycle, request.object) ||
        request.endpoints.empty())
        return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(request.object);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        request.moduleIndex >= component->chinookAi.size() ||
        request.moduleIndex >= component->plan->chinookAi.size())
        return false;

    ObjectChinookAiRuntime& runtime =
        component->chinookAi[request.moduleIndex];
    const game::ObjectChinookAiRule& rule =
        component->plan->chinookAi[request.moduleIndex];
    for (const ObjectChinookAiRuntime::Rope& oldRope : runtime.ropes)
        outEvents.push_back(chinookRopeEvent(
            ObjectChinookRopePresentationControl::End, request.object, rule,
            oldRope, request.confirmedTick));
    runtime.ropes.clear();
    runtime.pendingRappeller = INVALID_OBJECT_ID;
    runtime.pendingRopeIndex = 0;
    runtime.pendingEventSequence = 0;
    runtime.combatDropActive = false;

    ++runtime.ropeGeneration;
    if (runtime.ropeGeneration == 0)
        ++runtime.ropeGeneration;
    const size_t ropeCount = std::min<size_t>(
        request.endpoints.size(), static_cast<size_t>(rule.numRopes));
    if (ropeCount == 0)
    {
        runtime.ropesDropping = false;
        runtime.ropeReadyTicks.clear();
        return false;
    }
    runtime.ropes.reserve(ropeCount);
    runtime.ropeReadyTicks.assign(ropeCount, request.confirmedTick);
    const math::q32_32 gravityPerFrame = ropeGravityPerFrame(rules);
    runtime.rappelSpeedPerFrame =
        rule.rappelSpeedPerLegacyFrameFixed > math::q32_32{}
        ? legacyAuthoredPerFrameAtSessionRate(
              rule.rappelSpeedPerLegacyFrameFixed, rules)
        : math::q32_32::abs(gravityPerFrame) *
              math::q32_32{static_cast<int32_t>(std::max<uint32_t>(
                  1u, rules.logicFramesPerSecond))} *
              math::q32_32::from_fraction(1, 2);
    for (size_t index = 0; index < ropeCount; ++index)
    {
        ObjectChinookAiRuntime::Rope rope;
        rope.endpoint = request.endpoints[index];
        rope.ropeIndex = static_cast<uint32_t>(index);
        rope.identity = chinookRopeIdentity(
            request.object, rule.authoredOrder, runtime.ropeGeneration,
            rope.ropeIndex);
        rope.targetLength = rope.endpoint.ropeStart.z -
            rope.endpoint.surfaceHeight -
            rule.ropeFinalHeightFixed;
        rope.simulatedLength = math::q32_32{int32_t{1}};
        // initRopeParms initializes W3DRopeDraw::m_curLen to zero. Gameplay's
        // RopeInfo length starts at one and publishes it only on first update.
        rope.presentedLength = math::q32_32{};
        rope.lengthSpeedPerFrame = math::q32_32{};
        rope.dropSpeedPerFrame = math::q32_32::abs(
            legacyAuthoredPerFrameAtSessionRate(
                rule.ropeDropSpeedPerLegacyFrameFixed, rules));
        rope.wobbleRatePerFrame = legacyAuthoredPerFrameAtSessionRate(
            rule.ropeWobbleRatePerLegacyFrameFixed, rules);
        rope.lastUpdateTick = request.confirmedTick;
        rope.nextDropTick = saturatingAdd(
            request.confirmedTick,
            randomChinookDelayFrames(
                random, rule.perRopeDelayMinMilliseconds,
                rule.perRopeDelayMaxMilliseconds,
                rules.logicFramesPerSecond, true));
        runtime.ropeReadyTicks[index] = rope.nextDropTick;
        runtime.ropes.push_back(std::move(rope));
        outEvents.push_back(chinookRopeEvent(
            ObjectChinookRopePresentationControl::Begin, request.object, rule,
            runtime.ropes.back(), request.confirmedTick));
    }
    runtime.ropesDropping = true;
    runtime.combatDropActive = true;
    return true;
}

std::optional<ObjectChinookRopeReadyResult>
ObjectAirfieldSystem::nextReadyChinookRope(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, uint64_t confirmedTick) const
{
    if (!objectAlive(registry, lifecycle, object))
        return std::nullopt;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return std::nullopt;
    const ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->chinookAi.size() ||
        moduleIndex >= component->plan->chinookAi.size())
        return std::nullopt;
    const ObjectChinookAiRuntime& runtime = component->chinookAi[moduleIndex];
    const game::ObjectChinookAiRule& rule =
        component->plan->chinookAi[moduleIndex];
    if (!runtime.ropesDropping)
        return std::nullopt;
    for (const ObjectChinookAiRuntime::Rope& rope : runtime.ropes)
    {
        if (rope.released || confirmedTick < rope.nextDropTick ||
            (rule.waitForRopesToDrop &&
             rope.simulatedLength < rope.targetLength))
            continue;
        return ObjectChinookRopeReadyResult{
            .ropeIndex = rope.ropeIndex,
            .dropStart = rope.endpoint.dropStart,
            .dropOrientation = rope.endpoint.dropOrientation,
            .rappelSpeedPerFrame = runtime.rappelSpeedPerFrame,
        };
    }
    return std::nullopt;
}

bool ObjectAirfieldSystem::notifyChinookRappellerStarted(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, SimulationRandom& random,
    ObjectId object, size_t moduleIndex, size_t ropeIndex,
    uint64_t confirmedTick, ObjectId rappeller) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->chinookAi.size() ||
        moduleIndex >= component->plan->chinookAi.size())
        return false;
    ObjectChinookAiRuntime& runtime = component->chinookAi[moduleIndex];
    if (!runtime.ropesDropping || ropeIndex >= runtime.ropes.size() ||
        runtime.ropes[ropeIndex].released)
        return false;
    const game::ObjectChinookAiRule& rule =
        component->plan->chinookAi[moduleIndex];
    ObjectChinookAiRuntime::Rope& rope = runtime.ropes[ropeIndex];
    if (rappeller &&
        std::find(rope.rappellers.begin(), rope.rappellers.end(),
                  rappeller) == rope.rappellers.end()) {
        rope.rappellers.push_back(rappeller);
        std::sort(rope.rappellers.begin(), rope.rappellers.end());
    }
    rope.nextDropTick = saturatingAdd(
        confirmedTick,
        randomChinookDelayFrames(
            random, rule.perRopeDelayMinMilliseconds,
            rule.perRopeDelayMaxMilliseconds, rules.logicFramesPerSecond,
            false));
    if (ropeIndex < runtime.ropeReadyTicks.size())
        runtime.ropeReadyTicks[ropeIndex] = rope.nextDropTick;
    return true;
}

bool ObjectAirfieldSystem::endChinookCombatDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId object, size_t moduleIndex,
    uint64_t confirmedTick, bool immediate,
    container::Vector<ObjectChinookRopePresentationEvent>& outEvents) const
{
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->chinookAi.size() ||
        moduleIndex >= component->plan->chinookAi.size())
        return false;
    ObjectChinookAiRuntime& runtime = component->chinookAi[moduleIndex];
    const game::ObjectChinookAiRule& rule =
        component->plan->chinookAi[moduleIndex];
    if (!runtime.ropesDropping && runtime.ropes.empty())
        return false;
    runtime.ropesDropping = false;
    runtime.combatDropActive = false;
    runtime.pendingRappeller = INVALID_OBJECT_ID;
    runtime.pendingRopeIndex = 0;
    runtime.pendingEventSequence = 0;
    if (immediate)
    {
        for (const ObjectChinookAiRuntime::Rope& rope : runtime.ropes)
            outEvents.push_back(chinookRopeEvent(
                ObjectChinookRopePresentationControl::End, object, rule,
                rope, confirmedTick));
        runtime.ropes.clear();
        runtime.ropeReadyTicks.clear();
        return true;
    }

    const math::q32_32 gravityPerFrame = ropeGravityPerFrame(rules);
    const uint64_t lifetime = static_cast<uint64_t>(
        std::max<uint32_t>(1u, rules.logicFramesPerSecond)) * 5u;
    for (ObjectChinookAiRuntime::Rope& rope : runtime.ropes)
    {
        if (rope.released)
            continue;
        rope.released = true;
        rope.lastUpdateTick = confirmedTick;
        rope.expirationTick = saturatingAdd(confirmedTick, lifetime);
        rope.currentSpeedPerFrame = gravityPerFrame *
            math::q32_32{static_cast<int32_t>(std::max<uint32_t>(
                1u, rules.logicFramesPerSecond))};
        rope.maximumSpeedPerFrame = rope.dropSpeedPerFrame;
        rope.accelerationPerFrame = gravityPerFrame;
        outEvents.push_back(chinookRopeEvent(
            ObjectChinookRopePresentationControl::Update, object, rule, rope,
            confirmedTick));
    }
    return true;
}

} // namespace engine

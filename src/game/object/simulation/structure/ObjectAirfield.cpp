#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

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

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
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

void ObjectAirfieldSystem::initializeObject(ecs::registry& registry,
                                            ecs::entity entity,
                                            const ObjectSimulationRules& rules) const
{
    const ThingTemplateComponent* type = ecs::try_get<ThingTemplateComponent>(registry, entity);
    const auto plan = type && type->archetype ? type->archetype->airfieldPlan
                                              : container::SharedPtr<const game::ObjectAirfieldPlan>{};
    if (!plan)
        return;
    const ObjectLifecycleComponent* lifecycle = ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const uint64_t createdAtTick = lifecycle ? lifecycle->createdAtTick : 0u;
    ObjectAirfieldComponent component;
    component.plan = plan;
    component.initializedTick = createdAtTick;
    component.parkingPlaces.reserve(plan->parkingPlaces.size());
    for (const game::ObjectParkingPlaceRule& rule : plan->parkingPlaces)
    {
        const uint32_t spaceCount = static_cast<uint32_t>(std::max(0, rule.rows) * std::max(0, rule.cols));
        const uint32_t runwayCount = rule.hasRunways ? static_cast<uint32_t>(std::max(0, rule.cols)) : 0u;
        ObjectAirfieldParkingRuntime runtime;
        runtime.spaces.resize(spaceCount, INVALID_OBJECT_ID);
        runtime.runwayUsers.resize(runwayCount, INVALID_OBJECT_ID);
        runtime.nextTakeoffUsers.resize(runwayCount, INVALID_OBJECT_ID);
        runtime.nextHealTick = saturatingAdd(createdAtTick, millisecondsToFrames(200u, rules.logicFramesPerSecond));
        component.parkingPlaces.push_back(std::move(runtime));
    }
    component.flightDecks.reserve(plan->flightDecks.size());
    for (const game::ObjectFlightDeckRule& rule : plan->flightDecks)
    {
        const uint32_t runwayCount = static_cast<uint32_t>(std::max(0, rule.runways));
        uint32_t spaceCount = 0;
        for (const game::ObjectFlightDeckRunwayRule& runway : rule.runwayDefinitions)
        {
            const size_t authoredCount = runway.spaceBones.size();
            const uint64_t next =
                static_cast<uint64_t>(spaceCount) +
                (authoredCount != 0 ? authoredCount : static_cast<size_t>(std::max(0, rule.spacesPerRunway)));
            spaceCount = next >= std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                      : static_cast<uint32_t>(next);
        }
        if (rule.runwayDefinitions.empty())
        {
            const uint64_t fallbackCount =
                static_cast<uint64_t>(runwayCount) * static_cast<uint32_t>(std::max(0, rule.spacesPerRunway));
            spaceCount = fallbackCount >= std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                               : static_cast<uint32_t>(fallbackCount);
        }
        ObjectAirfieldFlightDeckRuntime runtime;
        runtime.spaces.resize(spaceCount, INVALID_OBJECT_ID);
        runtime.takeoffRunwayUsers.resize(runwayCount, INVALID_OBJECT_ID);
        runtime.landingRunwayUsers.resize(runwayCount, INVALID_OBJECT_ID);
        runtime.nextLaunchWaveTicks.resize(runwayCount, createdAtTick);
        runtime.rampReadyTicks.resize(runwayCount, 0u);
        runtime.catapultDueTicks.resize(
            runwayCount, std::numeric_limits<uint64_t>::max());
        runtime.lowerRampTicks.resize(
            runwayCount, std::numeric_limits<uint64_t>::max());
        runtime.rampRaised.resize(runwayCount, 0u);
        runtime.nextHealTick = saturatingAdd(createdAtTick, millisecondsToFrames(200u, rules.logicFramesPerSecond));
        runtime.nextCleanupTick =
            saturatingAdd(createdAtTick, millisecondsToFrames(rule.cleanupMilliseconds, rules.logicFramesPerSecond));
        runtime.nextAllowedProductionTick = createdAtTick;
        component.flightDecks.push_back(std::move(runtime));
    }
    component.jetAi.reserve(plan->jetAi.size());
    for (const game::ObjectJetAiRule& rule : plan->jetAi)
    {
        ObjectJetAiRuntime runtime;
        runtime.state = rule.needsRunway ? ObjectAircraftRuntimeState::Parked : ObjectAircraftRuntimeState::Airborne;
        runtime.phase = rule.needsRunway ? ObjectJetAirfieldPhase::Parked
                                         : ObjectJetAirfieldPhase::Airborne;
        runtime.phaseEnteredTick = createdAtTick;
        // These are state-entry deadlines, not object-age deadlines. The old
        // projection armed all three at construction, so a jet which spent a
        // long time parked skipped TakeoffPause/Lockon and immediately met
        // ReturnToBaseIdle when it finally became airborne.
        runtime.takeoffPauseUntilTick = 0;
        runtime.lockonReadyTick = 0;
        runtime.returnToBaseIdleDueTick = 0;
        component.jetAi.push_back(runtime);
    }
    component.chinookAi.reserve(plan->chinookAi.size());
    for (const game::ObjectChinookAiRule& rule : plan->chinookAi)
    {
        ObjectChinookAiRuntime runtime;
        runtime.ropeReadyTicks.resize(rule.numRopes, createdAtTick);
        const uint32_t minDelay = std::min(rule.perRopeDelayMinMilliseconds, rule.perRopeDelayMaxMilliseconds);
        const uint32_t maxDelay = std::max(rule.perRopeDelayMinMilliseconds, rule.perRopeDelayMaxMilliseconds);
        const uint32_t ropeStep = rule.numRopes > 1 ? (maxDelay - minDelay) / (rule.numRopes - 1u) : 0u;
        for (uint32_t index = 0; index < rule.numRopes; ++index)
        {
            runtime.ropeReadyTicks[index] = saturatingAdd(
                createdAtTick, chinookDelayToFrames(
                    minDelay + ropeStep * index,
                    rules.logicFramesPerSecond));
        }
        component.chinookAi.push_back(std::move(runtime));
    }
    component.spectreGunships.resize(plan->spectreGunships.size());
    component.spectreDeployments.resize(plan->spectreDeployments.size());
    component.slowDeaths.reserve(plan->slowDeaths.size());
    for (const game::ObjectAircraftSlowDeathRule& rule : plan->slowDeaths)
    {
        ObjectAircraftSlowDeathRuntime runtime;
        runtime.secondaryDueTick = saturatingAdd(
            createdAtTick, millisecondsToFrames(rule.delaySecondaryMilliseconds, rules.logicFramesPerSecond));
        runtime.finalBlowUpDueTick = saturatingAdd(
            createdAtTick, millisecondsToFrames(rule.delayFinalBlowUpMilliseconds, rules.logicFramesPerSecond));
        runtime.groundToFinalDueTick = saturatingAdd(
            createdAtTick,
            millisecondsToFrames(rule.delayFromGroundToFinalDeathMilliseconds, rules.logicFramesPerSecond));
        runtime.destroyDueTick = saturatingAdd(
            createdAtTick, millisecondsToFrames(rule.destructionDelayMilliseconds, rules.logicFramesPerSecond));
        const uint32_t bladeDelay = std::max(
            rule.minBladeFlyOffDelayMilliseconds,
            rule.maxBladeFlyOffDelayMilliseconds);
        runtime.bladeDetachDueTick = saturatingAdd(
            createdAtTick,
            millisecondsToFrames(bladeDelay, rules.logicFramesPerSecond));
        component.slowDeaths.push_back(runtime);
    }
    if (ObjectAirfieldComponent* existing = ecs::try_get<ObjectAirfieldComponent>(registry, entity))
    {
        *existing = std::move(component);
    }
    else
    {
        ecs::emplace<ObjectAirfieldComponent>(registry, entity, std::move(component));
    }
}

} // namespace engine

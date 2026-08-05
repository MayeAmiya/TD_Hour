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
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
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

bool ObjectAirfieldSystem::beginSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId object,
    size_t moduleIndex, LogicFixedVec3 initialTarget,
    LogicFixedVec3 overrideTarget, uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const
{
    if (!objectAlive(registry, lifecycle, object))
        return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->spectreGunships.size() ||
        moduleIndex >= component->plan->spectreGunships.size())
        return false;

    ObjectSpectreGunshipRuntime& runtime =
        component->spectreGunships[moduleIndex];
    const game::ObjectSpectreGunshipRule& rule =
        component->plan->spectreGunships[moduleIndex];
    if (runtime.targetingDecalsActive)
    {
        static_cast<void>(endSpectreGunshipTargeting(
            registry, lifecycle, rules, object, moduleIndex,
            confirmedTick, outEvents));
    }
    runtime.initialTargetPosition = initialTarget;
    runtime.overrideTargetDestination = constrainedSpectreTarget(
        initialTarget, overrideTarget, rule);
    runtime.gattlingTargetPosition = runtime.overrideTargetDestination;
    runtime.positionToShootAt = runtime.overrideTargetDestination;
    runtime.satellitePosition = spectrePosition(registry, *entity);
    runtime.departureTarget = {};
    runtime.currentTarget = INVALID_OBJECT_ID;
    runtime.gattling = INVALID_OBJECT_ID;
    runtime.howitzerFollowTicks = 0;
    runtime.nextHowitzerShotSequence = 1;
    runtime.cleanupRequested = false;
    runtime.phase = ObjectSpectreGunshipPhase::Inserting;
    runtime.phaseEnteredTick = confirmedTick;
    runtime.phaseEventPending = true;
    runtime.orbitEndsTick = saturatingAdd(
        confirmedTick, millisecondsToFrames(
            rule.orbitMilliseconds, rules.logicFramesPerSecond));
    runtime.nextHowitzerFireTick = saturatingAdd(
        confirmedTick, millisecondsToFrames(
            rule.howitzerFiringRateMilliseconds,
            rules.logicFramesPerSecond));
    runtime.targetingDecalsActive = true;
    runtime.state = ObjectAircraftRuntimeState::Airborne;
    publishSpectreModelState(
        registry, *entity, runtime.phase, confirmedTick,
        rule.authoredOrder);

    if (!rule.attackAreaDecal.texture.empty() &&
        rule.attackAreaRadiusFixed > math::q32_32{} &&
        rule.attackAreaDecal.shadowTypeMask != 0)
    {
        outEvents.push_back(spectreDecalEvent(
            ObjectRadiusDecalEventKind::Begin,
            ObjectRadiusDecalEventSource::SpectreAttackArea,
            registry, *entity, object, rule, rule.attackAreaDecal,
            runtime.initialTargetPosition, rule.attackAreaRadiusFixed,
            rules, confirmedTick));
    }
    if (!rule.targetingReticleDecal.texture.empty() &&
        rule.targetingReticleRadiusFixed > math::q32_32{} &&
        rule.targetingReticleDecal.shadowTypeMask != 0)
    {
        outEvents.push_back(spectreDecalEvent(
            ObjectRadiusDecalEventKind::Begin,
            ObjectRadiusDecalEventSource::SpectreTargetingReticle,
            registry, *entity, object, rule,
            rule.targetingReticleDecal,
            runtime.overrideTargetDestination,
            rule.targetingReticleRadiusFixed, rules, confirmedTick));
    }
    return true;
}

void ObjectAirfieldSystem::emitSpectreSpecialPowerSpawns(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules,
    const ObjectSpecialPowerExecutionEvent& event,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectSpecialPowerSpawnRequest>& outRequests,
    container::Vector<ObjectRadiusDecalEvent>& outDecalEvents) const {
    if (event.status != ObjectSpecialPowerExecutionStatus::Activated ||
        !event.hasTargetPosition || !event.source || !event.content) {
        return;
    }
    const SpecialPowerDefinition* definition =
        content.findSpecialPower(event.content);
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(event.source);
    const ObjectAirfieldComponent* airfield = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    const TransformComponent* transform = entity
        ? ecs::try_get<TransformComponent>(registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(registry, *entity)
        : nullptr;
    const PrimaryTeamComponent* team = entity
        ? ecs::try_get<PrimaryTeamComponent>(registry, *entity)
        : nullptr;
    if (!definition || !airfield || !airfield->plan || !transform ||
        !owner || !team || !team->team) {
        return;
    }

    const auto reserveEmission = [&]() {
        const uint64_t result = nextEmissionSequence;
        ++nextEmissionSequence;
        if (nextEmissionSequence == 0) ++nextEmissionSequence;
        return result;
    };
    const LogicFixedVec3 sourcePosition =
        readAuthoritativeObjectPosition(registry, *entity, *transform);

    const auto edgePoint = [&](container::StringView createLocation) {
        const game::terrain::TerrainExtentRaw extent =
            terrain.map().extentIncludingBorderRaw();
        const math::q32_32 minimumX =
            math::q32_32::from_raw(extent.minimumX);
        const math::q32_32 minimumY =
            math::q32_32::from_raw(extent.minimumY);
        const math::q32_32 maximumX =
            math::q32_32::from_raw(extent.maximumX);
        const math::q32_32 maximumY =
            math::q32_32::from_raw(extent.maximumY);
        const bool sourceAnchor = container::asciiEqualIgnoreCase(
                createLocation, "CREATE_AT_EDGE_NEAR_SOURCE") ||
            container::asciiEqualIgnoreCase(
                createLocation, "CREATE_AT_EDGE_FARTHEST_FROM_SOURCE");
        const bool farthest = container::asciiEqualIgnoreCase(
                createLocation, "CREATE_AT_EDGE_FARTHEST_FROM_SOURCE") ||
            container::asciiEqualIgnoreCase(
                createLocation, "CREATE_AT_EDGE_FARTHEST_FROM_TARGET");
        const LogicFixedVec3& anchor = sourceAnchor
            ? sourcePosition : event.targetPosition;
        const math::q32_32 clampedX = math::q32_32::max(
            minimumX, math::q32_32::min(maximumX, anchor.x));
        const math::q32_32 clampedY = math::q32_32::max(
            minimumY, math::q32_32::min(maximumY, anchor.y));
        const container::Array<LogicFixedVec3, 4> candidates{{
            {minimumX, clampedY, {}},
            {maximumX, clampedY, {}},
            {clampedX, minimumY, {}},
            {clampedX, maximumY, {}},
        }};
        size_t selected = 0;
        math::q32_32 selectedDistance{};
        for (size_t index = 0; index < candidates.size(); ++index) {
            const math::q32_32 dx = candidates[index].x - anchor.x;
            const math::q32_32 dy = candidates[index].y - anchor.y;
            const math::q32_32 distance = dx * dx + dy * dy;
            if (index == 0 || (farthest
                    ? distance > selectedDistance
                    : distance < selectedDistance)) {
                selected = index;
                selectedDistance = distance;
            }
        }
        return candidates[selected];
    };

    const auto deployment = std::find_if(
        airfield->plan->spectreDeployments.begin(),
        airfield->plan->spectreDeployments.end(),
        [&](const game::ObjectSpectreDeploymentRule& rule) {
            return container::asciiEqualIgnoreCase(
                       rule.specialPowerTemplate, definition->name) &&
                (event.updateAuthoredOrder == UINT32_MAX ||
                 event.updateAuthoredOrder == rule.authoredOrder);
        });
    if (deployment != airfield->plan->spectreDeployments.end() &&
        !deployment->gunshipTemplateName.empty() &&
        content.findObjectArchetype(deployment->gunshipTemplateName)) {
        const LogicFixedVec3 creation = edgePoint(deployment->createLocation);
        outRequests.push_back({
            .source = event.source,
            .owner = owner->player,
            .primaryTeam = team->team,
            .objectTemplate = deployment->gunshipTemplateName,
            .position = creation,
            .targetPosition = event.targetPosition,
            .yawRadians = math::fixed_atan2(
                event.targetPosition.y - creation.y,
                event.targetPosition.x - creation.x),
            .specialPower = event.content,
            .authoredOrder = deployment->authoredOrder,
            .emissionSequence = reserveEmission(),
            .confirmedTick = event.confirmedTick,
            .hasEffectPosition = true,
            .projectPreferredHeight = true,
            .markAirborne = true,
            .issueSpecialPowerOrder = true,
        });
    }

    for (size_t moduleIndex = 0;
         moduleIndex < airfield->plan->spectreGunships.size();
         ++moduleIndex) {
        const game::ObjectSpectreGunshipRule& rule =
            airfield->plan->spectreGunships[moduleIndex];
        if (!container::asciiEqualIgnoreCase(
                rule.specialPowerTemplate, definition->name)) {
            continue;
        }
        const ObjectId previousGattling =
            moduleIndex < airfield->spectreGunships.size()
            ? airfield->spectreGunships[moduleIndex].gattling
            : INVALID_OBJECT_ID;
        if (!beginSpectreGunshipTargeting(
                registry, lifecycle, rules, event.source, moduleIndex,
                event.targetPosition, event.targetPosition,
                event.confirmedTick, outDecalEvents)) {
            continue;
        }
        uint32_t containmentRuleIndex =
            std::numeric_limits<uint32_t>::max();
        const ObjectContainmentRuntimeComponent* containment =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                registry, *entity);
        if (containment && containment->plan) {
            for (size_t ruleIndex = 0;
                 ruleIndex < containment->plan->rules.size();
                 ++ruleIndex) {
                if (containment->plan->rules[ruleIndex].kind ==
                    ObjectContainmentKind::Helix) {
                    containmentRuleIndex =
                        static_cast<uint32_t>(ruleIndex);
                    break;
                }
            }
        }
        outRequests.push_back({
            .source = event.source,
            .owner = owner->player,
            .primaryTeam = team->team,
            .objectTemplate = rule.gattlingTemplateName,
            .position = sourcePosition,
            .yawRadians = readAuthoritativeObjectYaw(
                registry, *entity, *transform),
            .specialPower = event.content,
            .replacedObject = previousGattling,
            .completion = ObjectSpecialPowerSpawnCompletionKind::
                AirfieldCapabilityChild,
            .capabilityRuleIndex = static_cast<uint32_t>(moduleIndex),
            .containmentRuleIndex = containmentRuleIndex,
            .authoredOrder = rule.authoredOrder,
            .emissionSequence = reserveEmission(),
            .confirmedTick = event.confirmedTick,
            .attachToSourceContainment = true,
        });
    }
}

bool ObjectAirfieldSystem::assignSpectreGunshipGattling(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, ObjectId gattling,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    const std::optional<ecs::entity> gattlingEntity =
        lifecycle.entityFromId(gattling);
    if (!entity || !gattlingEntity || lifecycle.isPendingDestroy(object) ||
        lifecycle.isPendingDestroy(gattling)) {
        return false;
    }
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->spectreGunships.size() ||
        moduleIndex >= component->plan->spectreGunships.size()) {
        return false;
    }
    ObjectSpectreGunshipRuntime& runtime =
        component->spectreGunships[moduleIndex];
    runtime.gattling = gattling;
    if (runtime.phase == ObjectSpectreGunshipPhase::Inserting) {
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, *gattlingEntity, ObjectDisabledReason::Paralyzed,
            OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
    }
    return true;
}

bool ObjectAirfieldSystem::updateSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId object,
    size_t moduleIndex, LogicFixedVec3 overrideTarget,
    uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object))
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->spectreGunships.size() ||
        moduleIndex >= component->plan->spectreGunships.size())
        return false;
    ObjectSpectreGunshipRuntime& runtime =
        component->spectreGunships[moduleIndex];
    if (!runtime.targetingDecalsActive)
        return false;
    const game::ObjectSpectreGunshipRule& rule =
        component->plan->spectreGunships[moduleIndex];
    runtime.overrideTargetDestination = constrainedSpectreTarget(
        runtime.initialTargetPosition, overrideTarget, rule);

    if (!rule.attackAreaDecal.texture.empty() &&
        rule.attackAreaRadiusFixed > math::q32_32{})
    {
        outEvents.push_back(spectreDecalEvent(
            ObjectRadiusDecalEventKind::Update,
            ObjectRadiusDecalEventSource::SpectreAttackArea,
            registry, *entity, object, rule, rule.attackAreaDecal,
            runtime.initialTargetPosition, rule.attackAreaRadiusFixed,
            rules, confirmedTick));
    }
    if (!rule.targetingReticleDecal.texture.empty() &&
        rule.targetingReticleRadiusFixed > math::q32_32{})
    {
        outEvents.push_back(spectreDecalEvent(
            ObjectRadiusDecalEventKind::Update,
            ObjectRadiusDecalEventSource::SpectreTargetingReticle,
            registry, *entity, object, rule,
            rule.targetingReticleDecal,
            runtime.overrideTargetDestination,
            rule.targetingReticleRadiusFixed, rules, confirmedTick));
    }
    return true;
}

bool ObjectAirfieldSystem::endSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId object,
    size_t moduleIndex, uint64_t confirmedTick,
    container::Vector<ObjectRadiusDecalEvent>& outEvents) const
{
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan ||
        moduleIndex >= component->spectreGunships.size() ||
        moduleIndex >= component->plan->spectreGunships.size())
        return false;
    ObjectSpectreGunshipRuntime& runtime =
        component->spectreGunships[moduleIndex];
    if (!runtime.targetingDecalsActive)
        return false;
    const game::ObjectSpectreGunshipRule& rule =
        component->plan->spectreGunships[moduleIndex];

    outEvents.push_back(spectreDecalEvent(
        ObjectRadiusDecalEventKind::End,
        ObjectRadiusDecalEventSource::SpectreAttackArea,
        registry, *entity, object, rule, rule.attackAreaDecal,
        runtime.initialTargetPosition, rule.attackAreaRadiusFixed,
        rules, confirmedTick));
    outEvents.push_back(spectreDecalEvent(
        ObjectRadiusDecalEventKind::End,
        ObjectRadiusDecalEventSource::SpectreTargetingReticle,
        registry, *entity, object, rule, rule.targetingReticleDecal,
        runtime.overrideTargetDestination, rule.targetingReticleRadiusFixed,
        rules, confirmedTick));
    runtime.targetingDecalsActive = false;
    if (runtime.state == ObjectAircraftRuntimeState::Attacking)
        runtime.state = ObjectAircraftRuntimeState::ReturningToBase;
    return true;
}

} // namespace engine

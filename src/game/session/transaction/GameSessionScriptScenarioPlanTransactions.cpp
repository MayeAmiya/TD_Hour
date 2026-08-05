#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"

#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/script/GameSessionScriptTeamsDetail.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/command/OrderExecutor.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>
#include <iterator>
#include <variant>

namespace engine {
using namespace script_team_detail;
using namespace object_lifecycle_detail;

namespace {

void normalizeScenarioTeamJoinOrder(
    ObjectOrderIntent& order, PlayerId owner, uint64_t tick,
    uint32_t sequence) noexcept {
    order.source = ObjectOrderSource::System;
    order.contextPlayer = owner;
    order.issuedTick = tick;
    order.sourceSequence = sequence;
    order.sourceScriptId = 0;
    order.systemPurpose = ObjectOrderSystemPurpose::Generic;
    order.systemPurposeInstance = 0;

    // Retail joinTeam() clears the joining actor's state machine and
    // GoalWaypoint before matching the live teammate goal.  A copied modern
    // queue order must therefore not retain an authored waypoint route or a
    // player group-path correlation from the teammate.  The authoritative
    // fixed-point target itself remains intact.
    if (order.kind == ObjectOrderKind::Move)
        order.moveRouteSubtype = ObjectMoveRouteSubtype::Direct;
    order.waypointStartId = std::numeric_limits<uint32_t>::max();
    order.waypointGraphRevision = 0;
    order.waypointTeam = INVALID_OBJECT_TEAM_ID;
    order.waypointGroupOffsetX = {};
    order.waypointGroupOffsetY = {};
    order.waypointGroupSpeed = {};
    order.groupPathId = 0;
    order.groupPathMemberOrdinal = 0;
    order.groupPathMemberCount = 0;
    order.groupPathStartX = {};
    order.groupPathStartY = {};
    order.groupPathStartZ = {};
    order.groupPathOffsetX = {};
    order.groupPathOffsetY = {};
}

} // namespace

GameSessionScriptScenarioPlanTransactions::GameSessionScriptScenarioPlanTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionScenarioTransactionPort port) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_port(port) {}

bool GameSessionScriptScenarioPlanTransactions::createScriptReinforcementTeam(
    container::StringView teamName,
    container::StringView destinationWaypointName,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || teamName.empty() ||
        destinationWaypointName.empty() || !m_presentation.m_scenarioDefinition) {
        return false;
    }
    const game::terrain::WaypointRecord* destinationWaypoint =
        m_content.m_terrain.waypointByName(destinationWaypointName);
    const scenario::ScriptTeamDefinition* definition =
        m_port.findTeam(teamName);
    if (!destinationWaypoint || !definition ||
        !m_content.m_players.get(definition->resolvedOwner)) {
        return false;
    }

    const scenario::ScenarioTeamPlan& plan = definition->plan;
    const auto fixedWaypoint = [](const game::terrain::WaypointRecord& waypoint) {
        return LogicFixedVec3{
            math::q32_32::from_raw(waypoint.positionRaw[0]),
            math::q32_32::from_raw(waypoint.positionRaw[1]),
            math::q32_32::from_raw(waypoint.positionRaw[2]),
        };
    };
    const LogicFixedVec3 destination = fixedWaypoint(*destinationWaypoint);
    LogicFixedVec3 origin = destination;
    if (!plan.reinforcementOriginWaypoint.empty()) {
        if (const game::terrain::WaypointRecord* authoredOrigin =
                m_content.m_terrain.waypointByName(
                    plan.reinforcementOriginWaypoint)) {
            origin = fixedWaypoint(*authoredOrigin);
        }
    }
    const LogicFixedVec3 reinforcementOrigin = origin;
    const bool needsDestinationMove =
        origin.x != destination.x || origin.y != destination.y;

    std::optional<ObjectTeamId> team;
    if (definition->isSingleton) {
        team = m_world.m_objectTeams.scenarioTeam(definition->id);
    }
    if (!team) {
        team = m_world.m_objectTeams.createScenarioTeamInstance(
            definition->id, definition->name, definition->resolvedOwner,
            false);
    }
    if (!team) return false;

    const PlayerState* ownerState = m_content.m_players.get(definition->resolvedOwner);
    const UpgradeMask ownerUpgrades = ownerState
        ? ownerState->upgrades.completed
        : UpgradeMask{};
    const game::ObjectVeterancyLevel teamVeterancy =
        static_cast<game::ObjectVeterancyLevel>(std::clamp<int32_t>(
            plan.veterancy, 0,
            static_cast<int32_t>(game::ObjectVeterancyLevel::Heroic)));

    const auto spawnMember = [&](container::StringView templateName,
                                  const LogicFixedVec3& position)
        -> GameSessionObjectSpawnResult {
        if (templateName.empty()) return {};
        ObjectSpawnRequest request;
        request.templateName = container::String{templateName};
        request.owner = definition->resolvedOwner;
        request.primaryTeam = *team;
        request.transform = ObjectFixedTransformComponent{
            .position = position,
            .authoritative = true,
        };
        request.origin = ObjectCreationOrigin::Script;
        request.confirmedTick = confirmedTick;
        GameSessionObjectSpawnResult result =
            m_port.lifecycle.spawnObject(std::move(request));
        if (result && teamVeterancy != game::ObjectVeterancyLevel::Regular) {
            static_cast<void>(m_world.m_objectSimulation.setObjectVeterancyLevel(
                m_world.m_registry, m_world.m_objects, result.object, teamVeterancy,
                ownerUpgrades, confirmedTick,
                {.players = &m_content.m_players,
                 .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
                 .content = &m_content.m_contentSnapshot,
                 .random = &m_content.m_simulationRandom,
                 .effects = &m_world.m_objectSimulation}));
        }
        return result;
    };

    container::Vector<ObjectId> reinforcementTransports;
    const container::SharedPtr<const game::ObjectArchetype>
        reinforcementTransportArchetype =
            plan.reinforcementTransport.empty()
                ? container::SharedPtr<const game::ObjectArchetype>{}
                : m_content.m_contentSnapshot.findObjectArchetype(
                      plan.reinforcementTransport);
    const auto spawnReinforcementTransport = [&](uint32_t ordinal)
        -> ObjectId {
        if (!reinforcementTransportArchetype) return INVALID_OBJECT_ID;
        LogicFixedVec3 position = reinforcementOrigin;
        position.x += math::q32_32{static_cast<int32_t>(ordinal)} *
            math::q32_32::max(
                math::q32_32{int32_t{1}},
                reinforcementTransportArchetype->templateData.geometry.
                    majorRadiusFixed);
        position.z = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                position.x.raw(), position.y.raw()));
        GameSessionObjectSpawnResult created = spawnMember(
            reinforcementTransportArchetype->name, position);
        if (created) reinforcementTransports.push_back(created.object);
        return created.object;
    };

    // RefCode creates the first scripted transport before the payload roster,
    // which makes its stable ObjectId/member-list position observable.
    if (reinforcementTransportArchetype) {
        static_cast<void>(spawnReinforcementTransport(0));
    }

    container::Vector<ObjectId> rosterObjects;
    LogicFixedVec3 rosterOrigin = reinforcementOrigin;
    for (const scenario::ScenarioTeamUnitPlan& unit : plan.units) {
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            m_content.m_contentSnapshot.findObjectArchetype(unit.templateName);
        if (!archetype) continue;
        LogicFixedVec3 rowPosition = rosterOrigin;
        for (uint32_t index = 0; index < unit.maximumUnits; ++index) {
            LogicFixedVec3 position = rowPosition;
            position.x += math::q32_32::from_fraction(9, 4) *
                math::q32_32{static_cast<int32_t>(index)} *
                math::q32_32::max(
                    math::q32_32{},
                    archetype->templateData.geometry.majorRadiusFixed);
            position.z = math::q32_32::from_raw(
                m_content.m_terrain.groundHeightRaw(
                    position.x.raw(), position.y.raw()));
            GameSessionObjectSpawnResult created = spawnMember(
                archetype->name, position);
            if (created) rosterObjects.push_back(created.object);
        }
        rowPosition.y += math::q32_32{int32_t{2}} *
            math::q32_32::max(
                math::q32_32{},
                archetype->templateData.geometry.majorRadiusFixed);
        rosterOrigin.y = rowPosition.y;
    }

    const auto entityOf = [&](ObjectId object) {
        return m_world.m_objects.entityFromId(object);
    };
    const auto isContained = [&](ObjectId object) {
        const std::optional<ecs::entity> entity = entityOf(object);
        return entity && ecs::try_get<ObjectContainedByComponent>(
            m_world.m_registry, *entity) != nullptr;
    };
    const auto isTransportKind = [&](ObjectId object) {
        const std::optional<ecs::entity> entity = entityOf(object);
        const ObjectKindOfComponent* kinds = entity
            ? ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity)
            : nullptr;
        return kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Transport);
    };
    const auto attach = [&](ObjectId transport, ObjectId passenger,
                            bool force = false) {
        return m_world.m_objectSimulation.requestContainment(
            m_world.m_registry, m_world.m_objects,
            {.kind = ObjectContainmentRequestKind::Attach,
             .container = transport,
             .object = passenger,
             .confirmedTick = confirmedTick,
             .force = force},
            &m_content.m_players, &m_content.m_contentSnapshot);
    };

    // teamStartsFull partitions ordinary members into transports authored as
    // part of the roster before the special reinforcement transport is used.
    if (plan.startsFull) {
        container::Vector<ObjectId> rosterTransports;
        for (const ObjectId member : rosterObjects) {
            if (isTransportKind(member))
                rosterTransports.push_back(member);
        }
        for (const ObjectId member : rosterObjects) {
            if (isTransportKind(member) || isContained(member)) continue;
            for (const ObjectId transport : rosterTransports) {
                if (attach(transport, member)) break;
            }
        }
    }

    const auto transportBehaviorRule = [&](ObjectId transport)
        -> const ObjectTransportBehaviorRule* {
        const std::optional<ecs::entity> entity = entityOf(transport);
        const ObjectContainmentRuntimeComponent* runtime = entity
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (!runtime || !runtime->plan) return nullptr;
        const auto found = std::find_if(
            runtime->plan->behaviorRules.begin(),
            runtime->plan->behaviorRules.end(),
            [](const ObjectTransportBehaviorRule& rule) {
                return rule.kind ==
                    ObjectTransportBehaviorKind::DeliverPayloadAI;
            });
        return found == runtime->plan->behaviorRules.end()
            ? nullptr : &*found;
    };
    const ObjectTransportBehaviorRule* deliveryRule =
        reinforcementTransports.empty()
            ? nullptr
            : transportBehaviorRule(reinforcementTransports.front());

    if (!reinforcementTransports.empty()) {
        for (const ObjectId member : rosterObjects) {
            if (isContained(member)) continue;
            const std::optional<ecs::entity> memberEntity = entityOf(member);
            const ThingTemplateComponent* memberType = memberEntity
                ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry,
                                                        *memberEntity)
                : nullptr;
            if (!memberType || !memberType->archetype ||
                game::legacyThingTemplatesEquivalent(
                    memberType->archetype->templateData,
                    reinforcementTransportArchetype->templateData)) {
                continue;
            }

            ObjectId selectedTransport = INVALID_OBJECT_ID;
            for (const ObjectId candidate : reinforcementTransports) {
                if (m_world.m_objectSimulation.canContain(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = candidate,
                         .object = member,
                         .confirmedTick = confirmedTick},
                        &m_content.m_players)) {
                    selectedTransport = candidate;
                    break;
                }
            }
            if (!selectedTransport) {
                const ObjectId candidate = spawnReinforcementTransport(
                    static_cast<uint32_t>(reinforcementTransports.size()));
                if (candidate && m_world.m_objectSimulation.canContain(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = candidate,
                         .object = member,
                         .confirmedTick = confirmedTick},
                        &m_content.m_players)) {
                    selectedTransport = candidate;
                }
            }
            if (!selectedTransport) continue;

            ObjectId carriedObject = member;
            if (deliveryRule && deliveryRule->putInContainer &&
                !deliveryRule->payloadTemplate.empty()) {
                const std::optional<ecs::entity> transportEntity =
                    entityOf(selectedTransport);
                const ObjectFixedTransformComponent* transportTransform =
                    transportEntity
                    ? ecs::try_get<ObjectFixedTransformComponent>(
                          m_world.m_registry,
                          *transportEntity)
                    : nullptr;
                const LogicFixedVec3 wrapperPosition =
                    transportTransform && transportTransform->authoritative
                    ? transportTransform->position
                    : reinforcementOrigin;
                GameSessionObjectSpawnResult wrapper = spawnMember(
                    deliveryRule->payloadTemplate, wrapperPosition);
                if (!wrapper || !attach(wrapper.object, member)) {
                    if (wrapper) static_cast<void>(m_port.lifecycle.requestDestroyObject(
                        wrapper.object, ObjectDestroyReason::System,
                        confirmedTick));
                    continue;
                }
                carriedObject = wrapper.object;
            }
            if (!attach(selectedTransport, carriedObject,
                        carriedObject != member) && carriedObject != member) {
                static_cast<void>(m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = carriedObject,
                     .object = member,
                     .confirmedTick = confirmedTick,
                     .force = true},
                    &m_content.m_players, &m_content.m_contentSnapshot));
                static_cast<void>(m_port.lifecycle.requestDestroyObject(
                    carriedObject, ObjectDestroyReason::System,
                    confirmedTick));
            }
        }
    }

    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    const auto queueMove = [&](ObjectId object, const LogicFixedVec3& target,
                               ObjectOrderSource source,
                               ObjectOrderSystemPurpose purpose,
                               uint32_t purposeInstance,
                               bool append) {
        const std::optional<ecs::entity> entity = entityOf(object);
        if (!entity || !ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *entity)) return false;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
        if (!queue)
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        if (!append) queue->orders.clear();
        queue->orders.push_back({
            .kind = ObjectOrderKind::Move,
            .source = source,
            .issuedTick = confirmedTick,
            .sourceSequence = sequence++,
            .targetX = target.x,
            .targetY = target.y,
            .targetZ = target.z,
            .hasTargetPosition = true,
            .systemPurpose = purpose,
            .systemPurposeInstance = purposeInstance,
        });
        ++queue->revision;
        if (source != ObjectOrderSource::System) {
            ++queue->externalRevision;
            if (queue->externalRevision == 0) ++queue->externalRevision;
            if (!append) {
                queue->replacementExternalRevision =
                    queue->externalRevision;
                queue->replacementExternalSource = source;
                queue->replacementExternalKind = ObjectOrderKind::Move;
            }
        }
        return true;
    };

    if (reinforcementTransports.empty()) {
        static_cast<void>(m_world.m_objectTeams.activate(*team, confirmedTick));
        if (needsDestinationMove) {
            for (const ObjectId member : rosterObjects) {
                if (isContained(member)) continue;
                static_cast<void>(queueMove(
                    member, destination, ObjectOrderSource::Script,
                    ObjectOrderSystemPurpose::Generic, 0, false));
            }
        }
        return true;
    }

    // Members rejected by transport eligibility/capacity travel on their own.
    for (const ObjectId member : rosterObjects) {
        if (isContained(member)) continue;
        static_cast<void>(queueMove(
            member, destination, ObjectOrderSource::Script,
            ObjectOrderSystemPurpose::Generic, 0, false));
    }

    for (const ObjectId transport : reinforcementTransports) {
        const ObjectTransportBehaviorRule* rule =
            transportBehaviorRule(transport);
        if (rule) {
            const uint64_t opacityTicksNumerator =
                static_cast<uint64_t>(rule->deliveryDecalOpacityThrobMilliseconds) *
                std::max<uint32_t>(
                    1u, m_world.m_objectSimulation.rules().logicFramesPerSecond);
            static_cast<void>(m_world.m_objectSimulation.requestTransportBehavior(
                m_world.m_registry, m_world.m_objects,
                {
                    .kind = ObjectTransportBehaviorRequestKind::DeliverPayload,
                    .object = transport,
                    .x = destination.x,
                    .y = destination.y,
                    .z = destination.z,
                    .deliveryDistance = rule->deliveryDistance,
                    .dropOffsetX = rule->dropOffsetX,
                    .dropOffsetY = rule->dropOffsetY,
                    .dropOffsetZ = rule->dropOffsetZ,
                    .dropVarianceX = rule->dropVarianceX,
                    .dropVarianceY = rule->dropVarianceY,
                    .dropVarianceZ = rule->dropVarianceZ,
                    .dropDelayMilliseconds = rule->dropDelayMilliseconds,
                    .maximumAttempts = rule->maximumAttempts,
                    .deliveryDecal = rule->deliveryDecal,
                    .deliveryDecalRadius = rule->deliveryDecalRadius,
                    .deliveryDecalShadowTypeMask =
                        rule->deliveryDecalShadowTypeMask,
                    .deliveryDecalMinimumOpacity =
                        rule->deliveryDecalMinimumOpacity,
                    .deliveryDecalMaximumOpacity =
                        rule->deliveryDecalMaximumOpacity,
                    .deliveryDecalOpacityThrobTicks =
                        rule->deliveryDecalOpacityThrobMilliseconds == 0
                            ? 0u
                            : std::max<uint64_t>(
                                  1u,
                                  (opacityTicksNumerator + 999u) / 1000u),
                    .deliveryDecalColor = rule->deliveryDecalColor,
                    .deliveryDecalUsesPlayerColor =
                        rule->deliveryDecalUsesPlayerColor,
                    .deliveryDecalOnlyVisibleToOwningPlayer =
                        rule->deliveryDecalOnlyVisibleToOwningPlayer,
                    .confirmedTick = confirmedTick,
                }));
            continue;
        }

        if (!queueMove(
                transport, destination, ObjectOrderSource::System,
                ObjectOrderSystemPurpose::ScenarioReinforcementDeliver,
                team->value, false)) {
            continue;
        }
        if (plan.transportsExit) {
            static_cast<void>(queueMove(
                transport, reinforcementOrigin, ObjectOrderSource::System,
                ObjectOrderSystemPurpose::ScenarioReinforcementExit,
                team->value, true));
        }
    }
    return true;
}

ObjectId GameSessionScriptScenarioPlanTransactions::recruitScenarioTeamUnit(
    ObjectTeamId targetTeam,
    const scenario::ScriptTeamDefinition& definition,
    const container::SharedPtr<const game::ObjectArchetype>& wanted,
    const LogicFixedVec3& home, math::q32_32 radiusSquared,
    uint32_t& sourceSequence, uint64_t confirmedTick,
    uint32_t productionRosterIndex, bool orderToHome)
{
    if (!targetTeam || !wanted || radiusSquared < math::q32_32{})
        return INVALID_OBJECT_ID;
    const int32_t targetPriority =
        m_world.m_objectTeams.productionPriority(targetTeam).value_or(
            definition.plan.productionPriority);
    const auto sourceRecruitable = [this](const ObjectTeamRecord& source) {
        bool value = source.kind == ObjectTeamKind::PlayerDefault;
        if (source.scenarioDefinition && m_presentation.m_scenarioDefinition) {
            if (const scenario::ScriptTeamDefinition* sourceDefinition =
                    m_presentation.m_scenarioDefinition->findScriptTeam(
                        source.scenarioDefinition)) {
                value = value || sourceDefinition->plan.aiRecruitable;
            }
        }
        return source.recruitableOverride.value_or(value);
    };

    ObjectId best = INVALID_OBJECT_ID;
    math::q32_32 bestDistanceSquared = radiusSquared;
    for (const ObjectTeamRecord& source : m_world.m_objectTeams.teams()) {
        if (!source.id || source.id == targetTeam || !source.active ||
            source.owner != definition.resolvedOwner ||
            m_world.m_objectTeams.productionPriority(source.id).value_or(
                source.productionPriority) >= targetPriority ||
            !sourceRecruitable(source)) {
            continue;
        }
        for (const ObjectId candidate : source.members.values()) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(candidate);
            if (!entity || m_world.m_objects.isPendingDestroy(candidate) ||
                isObjectDisabledBy(m_world.m_registry, *entity,
                    ObjectDisabledReason::Held, confirmedTick) ||
                ecs::try_get<ObjectContainedByComponent>(
                    m_world.m_registry, *entity)) {
                continue;
            }
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
            if (health && health->effectivelyDead) continue;
            const ObjectScriptPanelPolicyComponent* panel =
                ecs::try_get<ObjectScriptPanelPolicyComponent>(
                    m_world.m_registry, *entity);
            if (panel && !panel->aiRecruitable) continue;
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            if (!type || !type->archetype || !transform ||
                !game::legacyThingTemplatesEquivalent(
                    wanted->templateData,
                    type->archetype->templateData)) {
                continue;
            }
            const LogicFixedVec3 candidatePosition =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, *entity,
                    *transform);
            const math::q32_32 dx = home.x - candidatePosition.x;
            const math::q32_32 dy = home.y - candidatePosition.y;
            const math::q32_32 distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > radiusSquared ||
                (best && (distanceSquared > bestDistanceSquared ||
                 (distanceSquared == bestDistanceSquared &&
                  candidate > best)))) {
                continue;
            }
            best = candidate;
            bestDistanceSquared = distanceSquared;
        }
    }
    const std::optional<ObjectTeamId> previousTeam = best
        ? m_world.m_objectTeams.teamOf(best) : std::nullopt;
    if (!best || !m_port.transferObjectToTeam(
            best, targetTeam, confirmedTick)) {
        return INVALID_OBJECT_ID;
    }
    if (productionRosterIndex != UINT32_MAX &&
        !m_world.m_objectTeams.recordProductionUnitCompleted(
            targetTeam, productionRosterIndex)) {
        if (previousTeam) {
            static_cast<void>(m_port.transferObjectToTeam(
                best, *previousTeam, confirmedTick));
        }
        return INVALID_OBJECT_ID;
    }

    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(best);
    if (entity && ecs::try_get<ObjectLocomotionComponent>(
            m_world.m_registry, *entity)) {
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        }
        queue->orders.clear();
        if (orderToHome) {
            queue->orders.push_back({
                .kind = ObjectOrderKind::Move,
                .source = ObjectOrderSource::System,
                .issuedTick = confirmedTick,
                .sourceSequence = sourceSequence,
                .targetX = home.x,
                .targetY = home.y,
                .targetZ = home.z,
                .hasTargetPosition = true,
                .systemPurpose = ObjectOrderSystemPurpose::Generic,
            });
        } else {
            const container::Span<const ObjectId> teammates =
                m_world.m_objectTeams.legacyMembers(targetTeam);
            for (const ObjectId teammate : teammates) {
                if (!teammate || teammate == best) continue;
                const std::optional<ecs::entity> teammateEntity =
                    m_world.m_objects.entityFromId(teammate);
                if (!teammateEntity ||
                    m_world.m_objects.isPendingDestroy(teammate) ||
                    isObjectDisabledBy(
                        m_world.m_registry, *teammateEntity,
                        ObjectDisabledReason::Held, confirmedTick)) {
                    continue;
                }
                const ObjectHealthComponent* teammateHealth =
                    ecs::try_get<ObjectHealthComponent>(
                        m_world.m_registry, *teammateEntity);
                if (teammateHealth && teammateHealth->effectivelyDead)
                    continue;
                const ThingTemplateComponent* teammateType =
                    ecs::try_get<ThingTemplateComponent>(
                        m_world.m_registry, *teammateEntity);
                const std::optional<ai::ObjectAIActorStateView>
                    teammateActor = m_ai.m_objectAI.actorState(teammate);
                if (!teammateType || !teammateType->archetype ||
                    !teammateType->archetype->hasAiUpdate ||
                    !teammateActor) {
                    continue;
                }
                const TransformComponent* teammateTransform = teammateEntity
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry, *teammateEntity)
                    : nullptr;
                if (!teammateTransform) continue;
                const ObjectOrderQueueComponent* teammateQueue =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        m_world.m_registry, *teammateEntity);
                if (teammateQueue && !teammateQueue->orders.empty()) {
                    ObjectOrderIntent catchUp =
                        teammateQueue->orders.front();
                    normalizeScenarioTeamJoinOrder(
                        catchUp, definition.resolvedOwner,
                        confirmedTick, sourceSequence);
                    queue->orders.push_back(std::move(catchUp));
                } else {
                    const LogicFixedVec3 position =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry, *teammateEntity,
                            *teammateTransform);
                    queue->orders.push_back({
                        .kind = ObjectOrderKind::Move,
                        .source = ObjectOrderSource::System,
                        .issuedTick = confirmedTick,
                        .sourceSequence = sourceSequence,
                        .targetX = position.x,
                        .targetY = position.y,
                        .targetZ = position.z,
                        .hasTargetPosition = true,
                        .systemPurpose =
                            ObjectOrderSystemPurpose::Generic,
                    });
                }
                break;
            }
        }
        ++queue->revision;
    }
    if (sourceSequence != std::numeric_limits<uint32_t>::max())
        ++sourceSequence;
    return best;
}

bool GameSessionScriptScenarioPlanTransactions::reinforceScriptTeam(
    ObjectTeamId team, container::StringView productType,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port || !team || productType.empty() ||
        !m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick ||
        !m_presentation.m_scenarioDefinition) {
        return false;
    }
    const ObjectTeamRecord* record = m_world.m_objectTeams.find(team);
    const scenario::ScriptTeamDefinition* definition = record &&
            record->active && !record->members.empty() &&
            record->scenarioDefinition
        ? m_presentation.m_scenarioDefinition->findScriptTeam(
              record->scenarioDefinition)
        : nullptr;
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(productType);
    if (!definition || !definition->plan.automaticallyReinforce ||
        !product || definition->resolvedOwner != record->owner ||
        m_world.m_objectProduction.pendingUnitCountForTeam(
            m_world.m_registry, team, {}, UINT32_MAX) != 0) {
        return false;
    }

    uint32_t maximum = 0;
    for (const scenario::ScenarioTeamUnitPlan& unit :
         definition->plan.units) {
        const container::SharedPtr<const game::ObjectArchetype> wanted =
            m_content.m_contentSnapshot.findObjectArchetype(
                unit.templateName);
        if (wanted && game::legacyThingTemplatesEquivalent(
                wanted->templateData, product->templateData)) {
            maximum = unit.maximumUnits;
            break;
        }
    }
    if (maximum == 0) return false;
    uint32_t current = 0;
    for (const ObjectId member : record->members.values()) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (type && type->archetype &&
            game::legacyThingTemplatesEquivalent(
                type->archetype->templateData,
                product->templateData) &&
            current != std::numeric_limits<uint32_t>::max()) {
            ++current;
        }
    }
    if (current >= maximum) return false;

    LogicFixedVec3 home{};
    if (const game::terrain::WaypointRecord* waypoint =
            m_content.m_terrain.waypointByName(
                definition->plan.homeWaypoint)) {
        home = {
            math::q32_32::from_raw(waypoint->positionRaw[0]),
            math::q32_32::from_raw(waypoint->positionRaw[1]),
            math::q32_32::from_raw(waypoint->positionRaw[2]),
        };
    }
    const container::Span<const ObjectId> legacyMembers =
        m_world.m_objectTeams.legacyMembers(team);
    if (!legacyMembers.empty()) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(legacyMembers.front());
        const TransformComponent* transform = entity
            ? ecs::try_get<TransformComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (entity && transform) {
            home = readAuthoritativeObjectPosition(
                m_world.m_registry, *entity, *transform);
        }
    }

    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    const math::q32_32 recruitRadius = math::q32_32::max(
        math::q32_32{},
        m_content.m_objectSimulationRules.ai.maximumRecruitDistance);
    if (recruitScenarioTeamUnit(
            team, *definition, product, home,
            recruitRadius * recruitRadius, sequence,
            confirmedTick, UINT32_MAX, false)) {
        return true;
    }

    bool ignorePrerequisites = false;
    if (!m_port.productionPolicy.admitsObjectBuildability(
            definition->resolvedOwner, *product,
            ignorePrerequisites)) {
        return false;
    }
    container::Vector<ObjectId> producers;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const ObjectProductionComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        const ObjectProductionComponent& production =
            view.template get<const ObjectProductionComponent>(entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(
                m_world.m_registry, entity);
        if (!identity.id || owner.player != definition->resolvedOwner ||
            !production.jobs.empty() || !production.plan ||
            !production.exitPlan ||
            m_world.m_objects.isPendingDestroy(identity.id) ||
            (status && status->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction) |
                game::objectStatusBit(game::ObjectStatusFlag::Sold))) ||
            !canObjectBuildTemplate(
                m_world.m_registry, entity,
                m_content.m_contentSnapshot,
                m_presentation.m_scriptCommandBarOverrides,
                m_content.m_players, definition->resolvedOwner,
                *product, ignorePrerequisites)) {
            continue;
        }
        producers.push_back(identity.id);
    }
    std::sort(producers.begin(), producers.end());
    const std::optional<ObjectProductionRoutePoint> rally =
        ObjectProductionRoutePoint{.x = home.x, .y = home.y, .z = home.z};
    for (const ObjectId producer : producers) {
        const ObjectProductionRequestResult result =
            m_world.m_objectProduction.queueUnit(
                m_world.m_registry, m_world.m_objects,
                m_content.m_players, m_content.m_contentSnapshot,
                m_presentation.m_scriptCommandBarOverrides,
                producer, definition->resolvedOwner, product,
                confirmedTick, sequence,
                static_cast<uint32_t>(std::max(
                    1, m_content.m_startInfo.gameSpeedFPS)),
                m_content.m_objectSimulationRules.energy,
                ignorePrerequisites, team, rally, UINT32_MAX);
        if (result.accepted) return true;
        if (result.rejection ==
            ObjectProductionRejectionReason::InsufficientFunds) {
            return false;
        }
    }
    return false;
}

void GameSessionScriptScenarioPlanTransactions::
    updatePendingScenarioTeamReinforcements() {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame)
        return;
    struct PendingJoin final {
        ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
        ObjectId object = INVALID_OBJECT_ID;
    };
    container::Vector<PendingJoin> pending;
    for (const ObjectTeamRecord& team : m_world.m_objectTeams.teams()) {
        for (const ObjectId object : team.pendingReinforcements)
            pending.push_back({team.id, object});
    }
    const uint64_t tick = m_presentation.m_confirmedTick;
    for (const PendingJoin& join : pending) {
        const ObjectTeamRecord* team = m_world.m_objectTeams.find(join.team);
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(join.object);
        if (!team || !team->active || !entity ||
            !team->members.contains(join.object) ||
            m_world.m_objects.isPendingDestroy(join.object)) {
            static_cast<void>(m_world.m_objectTeams.removePendingReinforcement(
                join.team, join.object));
            continue;
        }
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        const std::optional<ai::ObjectAIActorStateView> actor =
            m_ai.m_objectAI.actorState(join.object);
        if (type && type->archetype && type->archetype->hasAiUpdate &&
            !actor) {
            // Lifecycle membership is committed after the production spawn;
            // wait for the actor rather than mistaking the one-tick gap for
            // a non-AI reinforcement.
            continue;
        }
        const ObjectOrderQueueComponent* currentQueue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        if ((currentQueue && !currentQueue->orders.empty()) ||
            (actor && !actor->idle)) {
            continue;
        }

        ObjectOrderQueueComponent* queue = currentQueue
            ? ecs::try_get<ObjectOrderQueueComponent>(
                  m_world.m_registry, *entity)
            : &ecs::emplace<ObjectOrderQueueComponent>(
                  m_world.m_registry, *entity);
        bool ordered = false;
        for (const ObjectId teammate : team->legacyMemberOrder) {
            if (!teammate || teammate == join.object) continue;
            const std::optional<ecs::entity> teammateEntity =
                m_world.m_objects.entityFromId(teammate);
            if (!teammateEntity ||
                m_world.m_objects.isPendingDestroy(teammate) ||
                isObjectDisabledBy(
                    m_world.m_registry, *teammateEntity,
                    ObjectDisabledReason::Held, tick)) {
                continue;
            }
            const ObjectHealthComponent* teammateHealth =
                ecs::try_get<ObjectHealthComponent>(
                    m_world.m_registry, *teammateEntity);
            if (teammateHealth && teammateHealth->effectivelyDead)
                continue;
            const ThingTemplateComponent* teammateType =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *teammateEntity);
            const std::optional<ai::ObjectAIActorStateView> teammateActor =
                m_ai.m_objectAI.actorState(teammate);
            if (!teammateType || !teammateType->archetype ||
                !teammateType->archetype->hasAiUpdate || !teammateActor) {
                continue;
            }
            const TransformComponent* teammateTransform = teammateEntity
                ? ecs::try_get<TransformComponent>(
                      m_world.m_registry, *teammateEntity)
                : nullptr;
            if (!teammateTransform) continue;
            const ObjectOrderQueueComponent* teammateQueue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry, *teammateEntity);
            if (teammateQueue && !teammateQueue->orders.empty()) {
                ObjectOrderIntent inherited = teammateQueue->orders.front();
                normalizeScenarioTeamJoinOrder(
                    inherited, team->owner, tick, join.object.value);
                queue->orders.push_back(std::move(inherited));
            } else {
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *teammateEntity,
                        *teammateTransform);
                queue->orders.push_back({
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .issuedTick = tick,
                    .sourceSequence = join.object.value,
                    .targetX = position.x,
                    .targetY = position.y,
                    .targetZ = position.z,
                    .hasTargetPosition = true,
                    .systemPurpose = ObjectOrderSystemPurpose::Generic,
                });
            }
            ordered = true;
            break;
        }
        if (ordered) {
            ++queue->revision;
        }
        static_cast<void>(m_world.m_objectTeams.removePendingReinforcement(
            join.team, join.object));
    }
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptTeam(
    container::StringView teamName, uint32_t sourceSequence,
    uint64_t confirmedTick, bool priorityBuild)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || teamName.empty() ||
        !m_presentation.m_scenarioDefinition) return false;
    const scenario::ScriptTeamDefinition* definition =
        m_port.findTeam(teamName);
    const PlayerState* player = definition
        ? m_content.m_players.get(definition->resolvedOwner) : nullptr;
    if (!definition || !player ||
        player->controller != PlayerControllerKind::Ai ||
        !player->constructionPolicy.unitConstructionEnabled) {
        return false;
    }

    std::optional<ObjectTeamId> team;
    if (definition->isSingleton) {
        team = m_world.m_objectTeams.scenarioTeam(definition->id);
        if (team && !m_world.m_objectTeams.members(*team).empty()) return false;
        const ObjectTeamRecord* existing = team
            ? m_world.m_objectTeams.find(*team) : nullptr;
        if (existing &&
            existing->assemblyKind != ObjectTeamAssemblyKind::None)
            return false;
    }

    // RefCode rejects the whole request when a required unit type has no
    // compatible factory/technology. A full queue or insufficient cash is
    // transient and is deliberately not part of this preflight.
    for (const scenario::ScenarioTeamUnitPlan& unitPlan :
         definition->plan.units) {
        const container::SharedPtr<const game::ObjectArchetype> product =
            m_content.m_contentSnapshot.findObjectArchetype(unitPlan.templateName);
        bool ignorePrerequisites = false;
        if (!product || !m_port.productionPolicy.admitsObjectBuildability(
                definition->resolvedOwner, *product,
                ignorePrerequisites)) {
            return false;
        }
        bool hasFactory = false;
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const OwnerComponent,
                                    const ObjectProductionComponent>(m_world.m_registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectProductionComponent& production =
                view.template get<const ObjectProductionComponent>(entity);
            if (!identity.id || owner.player != definition->resolvedOwner ||
                m_world.m_objects.isPendingDestroy(identity.id) ||
                !production.plan || !production.exitPlan) {
                continue;
            }
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
            if ((status && status->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::UnderConstruction) |
                    game::objectStatusBit(
                        game::ObjectStatusFlag::Sold)))) {
                continue;
            }
            if (canObjectBuildTemplate(
                    m_world.m_registry, entity, m_content.m_contentSnapshot,
                    m_presentation.m_scriptCommandBarOverrides, m_content.m_players,
                    definition->resolvedOwner, *product,
                    ignorePrerequisites)) {
                hasFactory = true;
                break;
            }
        }
        if (!hasFactory) return false;
    }

    if (team && m_world.m_objectTeams.isActive(*team))
        static_cast<void>(m_world.m_objectTeams.deactivate(*team));
    if (!team) {
        team = m_world.m_objectTeams.createScenarioTeamInstance(
            definition->id, definition->name,
            definition->resolvedOwner, false);
    }
    if (!team) return false;
    const uint64_t buildDeadline = definition->plan.initialIdleFrames < 1
        ? std::numeric_limits<uint64_t>::max()
        : confirmedTick > std::numeric_limits<uint64_t>::max() -
                static_cast<uint64_t>(definition->plan.initialIdleFrames)
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick +
                static_cast<uint64_t>(definition->plan.initialIdleFrames);
    if (!m_world.m_objectTeams.beginAssembly(
            *team, ObjectTeamAssemblyKind::Production, buildDeadline,
            sourceSequence == 0 ? 1u : sourceSequence,
            confirmedTick, priorityBuild)) {
        return false;
    }
    if (!m_world.m_objectTeams.initializeProductionProgress(
            *team, definition->plan.units.size())) {
        static_cast<void>(m_world.m_objectTeams.clearAssembly(*team));
        if (!definition->isSingleton)
            static_cast<void>(
                m_world.m_objectTeams.retireEmptyScenarioTeam(*team));
        return false;
    }
    static_cast<void>(m_world.m_objectTeams.markProductionStarted(
        *team, confirmedTick, 1u, true));
    // The effect occurs after the normal frame's assembly phase. Seed its
    // first recruit/queue pass now so BUILD_TEAM has no artificial one-frame
    // latency; later confirmed frames use the same derived planner.
    updateScenarioTeamProductions();
    return true;
}

bool GameSessionScriptScenarioPlanTransactions::recruitScriptTeam(
    container::StringView teamName, math::q32_32 radius,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick ||
        teamName.empty() || !m_presentation.m_scenarioDefinition) return false;
    const scenario::ScriptTeamDefinition* definition =
        m_port.findTeam(teamName);
    if (!definition || !m_content.m_players.get(definition->resolvedOwner)) return false;

    std::optional<ObjectTeamId> team;
    if (definition->isSingleton) {
        team = m_world.m_objectTeams.scenarioTeam(definition->id);
        if (team && !m_world.m_objectTeams.members(*team).empty()) return false;
        if (team && m_world.m_objectTeams.isActive(*team))
            static_cast<void>(m_world.m_objectTeams.deactivate(*team));
    }
    if (!team) {
        team = m_world.m_objectTeams.createScenarioTeamInstance(
            definition->id, definition->name,
            definition->resolvedOwner, false);
    }
    if (!team) return false;

    LogicFixedVec3 home{};
    if (const game::terrain::WaypointRecord* waypoint =
            m_content.m_terrain.waypointByName(definition->plan.homeWaypoint)) {
        home = {
            math::q32_32::from_raw(waypoint->positionRaw[0]),
            math::q32_32::from_raw(waypoint->positionRaw[1]),
            math::q32_32::from_raw(waypoint->positionRaw[2]),
        };
    } else {
        home.z = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(0, 0));
    }
    const math::q32_32 effectiveRadius = radius < math::q32_32{int32_t{1}}
        ? math::q32_32{int32_t{99999}}
        : radius;
    const math::q32_32 radiusSquared = effectiveRadius * effectiveRadius;
    uint32_t sequence = sourceSequence == 0 ? 1u : sourceSequence;
    size_t recruited = 0;

    for (const scenario::ScenarioTeamUnitPlan& unitPlan :
         definition->plan.units) {
        const container::SharedPtr<const game::ObjectArchetype> wanted =
            m_content.m_contentSnapshot.findObjectArchetype(unitPlan.templateName);
        if (!wanted) continue;
        for (uint32_t count = 0; count < unitPlan.maximumUnits; ++count) {
            if (!recruitScenarioTeamUnit(
                    *team, *definition, wanted, home, radiusSquared,
                    sequence, confirmedTick)) break;
            ++recruited;
        }
    }

    if (recruited == 0) {
        if (!definition->isSingleton)
            static_cast<void>(m_world.m_objectTeams.retireEmptyScenarioTeam(*team));
        return false;
    }
    const uint64_t timeoutFrames =
        static_cast<uint64_t>(std::max(
            1, m_content.m_startInfo.gameSpeedFPS)) * 60u;
    const uint64_t deadline = confirmedTick >
            std::numeric_limits<uint64_t>::max() - timeoutFrames
        ? std::numeric_limits<uint64_t>::max()
        : confirmedTick + timeoutFrames;
    return m_world.m_objectTeams.beginAssembly(
        *team, ObjectTeamAssemblyKind::Recruit, deadline,
        sourceSequence == 0 ? 1u : sourceSequence,
        confirmedTick);
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptPlayerUpgrade(
    PlayerId player, container::StringView upgradeName,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || upgradeName.empty()) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState) return false;

    // Player::buildUpgrade forwards only to AIPlayer. Human, observer and
    // neutral players silently ignore the authored request in RefCode.
    if (playerState->controller != PlayerControllerKind::Ai) return true;

    const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* upgrade = catalog
        ? catalog->find(upgradeName) : nullptr;
    if (!upgrade || upgrade->type != UpgradeDefinitionType::Player) {
        return true;
    }
    if (m_content.m_players.hasUpgradeComplete(player, upgrade->id) ||
        m_content.m_players.hasUpgradeInProgress(player, upgrade->id)) {
        return true;
    }

    // The old AI walks the player's BuildList and queues at the first factory
    // whose current CommandSet exposes this Upgrade. Modern ownership already
    // maintains a stable ObjectId-sorted index; queuePlayerUpgrade performs
    // the same frozen CommandButton, affordability, FIFO and technology
    // admission without duplicating those rules here.
    const container::Span<const ObjectId> owned = m_world.m_ownership.objects(player);
    const container::Vector<ObjectId> snapshot{owned.begin(), owned.end()};
    for (const ObjectId producer : snapshot) {
        if (!producer || m_world.m_objects.isPendingDestroy(producer)) continue;
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(producer);
        if (!entity) continue;
        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(m_world.m_registry, *entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
        if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive ||
            !ecs::try_get<ObjectProductionComponent>(m_world.m_registry, *entity) ||
            (status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
                game::objectStatusBit(game::ObjectStatusFlag::Sold)))) {
            continue;
        }
        if (m_port.orderPolicy.queuePlayerUpgrade(
                producer, player, upgrade->name, sourceSequence,
                confirmedTick,
                ObjectUpgradeProductionAdmission::ScriptAi).accepted) {
            return true;
        }
    }

    // Missing money/factory, a busy queue or unavailable prerequisites are
    // ordinary AI planning failures in RefCode: the one-shot script Action is
    // consumed and does not manufacture a retry or a completed upgrade.
    return true;
}


} // namespace engine

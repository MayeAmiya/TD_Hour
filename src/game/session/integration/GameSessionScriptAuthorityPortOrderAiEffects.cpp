#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptAuthorityPort.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "math/fixed/fixed_raw_mean.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {
[[nodiscard]] ObjectOrderKind toObjectOrderKind(ScriptOrderKind kind) noexcept {
    switch (kind) {
    case ScriptOrderKind::Move: return ObjectOrderKind::Move;
    case ScriptOrderKind::Stop: return ObjectOrderKind::Stop;
    case ScriptOrderKind::Attack: return ObjectOrderKind::Attack;
    case ScriptOrderKind::Build: return ObjectOrderKind::Build;
    case ScriptOrderKind::CommandButton: return ObjectOrderKind::CommandButton;
    case ScriptOrderKind::SpecialPower: return ObjectOrderKind::SpecialPower;
    case ScriptOrderKind::TacticalAttack: return ObjectOrderKind::TacticalAttack;
    }
    return ObjectOrderKind::Move;
}

[[nodiscard]] ObjectTacticalAttackSubtype toObjectTacticalAttackSubtype(
    ScriptTacticalAttackSubtype subtype) noexcept {
    switch (subtype) {
    case ScriptTacticalAttackSubtype::None:
        return ObjectTacticalAttackSubtype::None;
    case ScriptTacticalAttackSubtype::Hunt:
        return ObjectTacticalAttackSubtype::Hunt;
    case ScriptTacticalAttackSubtype::Guard:
        return ObjectTacticalAttackSubtype::Guard;
    case ScriptTacticalAttackSubtype::GuardTunnelNetwork:
        return ObjectTacticalAttackSubtype::GuardTunnelNetwork;
    case ScriptTacticalAttackSubtype::AttackSquad:
        return ObjectTacticalAttackSubtype::AttackSquad;
    case ScriptTacticalAttackSubtype::AttackArea:
        return ObjectTacticalAttackSubtype::AttackArea;
    }
    return ObjectTacticalAttackSubtype::None;
}

[[nodiscard]] ObjectMoveRouteSubtype toObjectMoveRouteSubtype(
    ScriptMoveRouteSubtype subtype) noexcept {
    switch (subtype) {
    case ScriptMoveRouteSubtype::Direct:
        return ObjectMoveRouteSubtype::Direct;
    case ScriptMoveRouteSubtype::WaypointPathIndividuals:
        return ObjectMoveRouteSubtype::WaypointPathIndividuals;
    case ScriptMoveRouteSubtype::WaypointPathTeam:
        return ObjectMoveRouteSubtype::WaypointPathTeam;
    case ScriptMoveRouteSubtype::WaypointPathIndividualsExact:
        return ObjectMoveRouteSubtype::WaypointPathIndividualsExact;
    case ScriptMoveRouteSubtype::WaypointPathTeamExact:
        return ObjectMoveRouteSubtype::WaypointPathTeamExact;
    case ScriptMoveRouteSubtype::WanderWaypointPath:
        return ObjectMoveRouteSubtype::WanderWaypointPath;
    case ScriptMoveRouteSubtype::PanicWaypointPath:
        return ObjectMoveRouteSubtype::PanicWaypointPath;
    }
    return ObjectMoveRouteSubtype::Direct;
}

} // namespace

using detail::kindOfContains;

namespace detail {

bool applyOrderAndAiEffect(
    GameSessionScriptAuthorityPort& bridge, const ScriptEffect& effect) {
    bool handled = false;
    std::visit([&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, ScriptFireWeaponFollowingWaypointPathEffect> ||
                      std::is_same_v<Payload, ScriptCreateReinforcementTeamEffect> ||
                      std::is_same_v<Payload, ScriptBuildTeamEffect> ||
                      std::is_same_v<Payload, ScriptGuardSupplyCenterEffect> ||
                      std::is_same_v<Payload, ScriptRecruitTeamEffect> ||
                      std::is_same_v<Payload, ScriptUseCommandButtonEffect> ||
                      std::is_same_v<Payload, ScriptOrderEffect> ||
                      std::is_same_v<Payload, ScriptAIBehaviorMutationEffect> ||
                      std::is_same_v<Payload, ScriptAttackPriorityMutationEffect> ||
                      std::is_same_v<Payload, ScriptFacingEffect>) {
            handled = true;
        if constexpr (std::is_same_v<
                                   Payload,
                                   ScriptFireWeaponFollowingWaypointPathEffect>) {
            if (!bridge.m_orderTransactions.fireWeaponFollowingWaypointPath(
                    payload.object, payload.waypointPathName,
                    effect.header.ordinal, effect.header.confirmedTick)) {
                // The legacy action is a silent no-op for a missing path,
                // incapable WeaponSet, unavailable weapon or missing Object.
                // Keep diagnostics detached from gameplay semantics.
                bridge.emitDiagnostic(
                    effect.header,
                    "Waypoint-following script weapon was not fired");
            }
        } else if constexpr (std::is_same_v<
                                   Payload,
                                   ScriptCreateReinforcementTeamEffect>) {
            if (!bridge.m_scenarioPlanTransactions.createScriptReinforcementTeam(
                    payload.teamName, payload.destinationWaypointName,
                    effect.header.ordinal, effect.header.confirmedTick)) {
                // RefCode silently returns for an unknown prototype or
                // destination.  Keep the gameplay no-op while retaining a
                // detached diagnostic for malformed map content.
                bridge.emitDiagnostic(
                    effect.header,
                    "Script reinforcement Team could not be created: " +
                        payload.teamName);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptBuildTeamEffect>) {
            if (!bridge.m_scenarioPlanTransactions.buildScriptTeam(
                    payload.teamName, effect.header.ordinal,
                    effect.header.confirmedTick, true)) {
                bridge.emitDiagnostic(
                    effect.header,
                    "Script Team production could not be started: " +
                        payload.teamName);
            }
        } else if constexpr (std::is_same_v<
                                   Payload,
                                   ScriptGuardSupplyCenterEffect>) {
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) return;
            if (!bridge.m_scenarioPlanTransactions.guardScriptTeamSupplyCenter(
                    *team, payload.minimumSupplies,
                    effect.header.sourceScript.value,
                    effect.header.ordinal,
                    effect.header.confirmedTick)) {
                bridge.emitDiagnostic(
                    effect.header,
                    "Script Team supply-center guard request was rejected: " +
                        payload.teamName);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptRecruitTeamEffect>) {
            if (!bridge.m_scenarioPlanTransactions.recruitScriptTeam(
                    payload.teamName, payload.radius,
                    effect.header.ordinal, effect.header.confirmedTick)) {
                bridge.emitDiagnostic(
                    effect.header,
                    "Script Team recruitment produced no assembly: " +
                        payload.teamName);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptUseCommandButtonEffect>) {
            container::Vector<ObjectId> actors = payload.actors;
            ScriptOrderAuthority authority =
                ScriptOrderAuthority::NamedObjects;
            ObjectTeamId scenarioTeam = INVALID_OBJECT_TEAM_ID;
            if (payload.actorSelector ==
                ScriptOrderActorSelector::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!team || !bridge.objectTeams().find(*team)) return;
                authority = ScriptOrderAuthority::ScenarioTeam;
                scenarioTeam = *team;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                actors.assign(members.begin(), members.end());
            }
            if (actors.empty()) return;

            ScriptCommandButtonTargetKind effectiveTargetKind =
                payload.targetKind;
            std::optional<ObjectId> effectiveTargetObject =
                payload.targetObject;
            if (payload.preselectSourceAndTarget) {
                const auto selection =
                    bridge.m_orderAdmissionTransactions.selectScriptCommandButtonExecution(
                        container::Span<const ObjectId>{actors},
                        payload.buttonName, payload.actorPolicy,
                        payload.actorPercentage,
                        payload.targetKind,
                        payload.targetObject.value_or(INVALID_OBJECT_ID),
                        payload.targetFilter,
                        container::Span<const container::String>{
                            payload.targetObjectTypes},
                        std::nullopt);
                if (!selection || selection->actors.empty()) return;
                actors = selection->actors;
                effectiveTargetObject = selection->targetObject
                    ? std::optional<ObjectId>{selection->targetObject}
                    : std::nullopt;
                effectiveTargetKind = effectiveTargetObject
                    ? ScriptCommandButtonTargetKind::NamedObject
                    : ScriptCommandButtonTargetKind::None;
            }

            CommandPosition targetPosition;
            if (effectiveTargetKind ==
                ScriptCommandButtonTargetKind::Waypoint) {
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().waypointByName(
                        payload.targetWaypointName);
                // RefCode returns before CommandButton lookup when the map
                // does not contain the authored waypoint.
                if (!waypoint) return;
                targetPosition = {
                    .x = math::q32_32::from_raw(waypoint->positionRaw[0]),
                    .y = math::q32_32::from_raw(waypoint->positionRaw[1]),
                    .z = math::q32_32::from_raw(waypoint->positionRaw[2]),
                    .valid = true,
                };
            } else if (effectiveTargetKind ==
                       ScriptCommandButtonTargetKind::WaypointPath) {
                // RefCode resolves the path label from the named unit's
                // current position, then fires the button at that waypoint.
                if (actors.size() != 1u) return;
                const std::optional<ecs::entity> actor =
                    bridge.entityFromId(actors.front());
                const TransformComponent* transform = actor
                    ? ecs::try_get<TransformComponent>(
                          bridge.registry(), *actor)
                    : nullptr;
                if (!transform) return;
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        bridge.registry(), *actor, *transform);
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().closestWaypointOnPathRaw(
                        position.x.raw(), position.y.raw(),
                        payload.targetWaypointName);
                if (!waypoint) return;
                targetPosition = {
                    .x = math::q32_32::from_raw(waypoint->positionRaw[0]),
                    .y = math::q32_32::from_raw(waypoint->positionRaw[1]),
                    .z = math::q32_32::from_raw(waypoint->positionRaw[2]),
                    .valid = true,
                };
            }
            const bool validTargetShape =
                (effectiveTargetKind == ScriptCommandButtonTargetKind::None &&
                 !effectiveTargetObject && !targetPosition.valid) ||
                (effectiveTargetKind ==
                     ScriptCommandButtonTargetKind::NamedObject &&
                 effectiveTargetObject && !targetPosition.valid) ||
                (effectiveTargetKind == ScriptCommandButtonTargetKind::Waypoint &&
                 !effectiveTargetObject && targetPosition.valid) ||
                (effectiveTargetKind == ScriptCommandButtonTargetKind::WaypointPath &&
                 !effectiveTargetObject && targetPosition.valid);
            if (!validTargetShape) {
                bridge.emitDiagnostic(effect.header,
                    "Rejected malformed CommandButton target shape");
                return;
            }

            const OrderExecutionResult result =
                bridge.m_orderAdmissionTransactions.executeScriptCommandButton({
                    .contextPlayer =
                        effect.header.invocation.currentPlayer,
                    .authority = authority,
                    .scenarioTeam = scenarioTeam,
                    .confirmedTick = effect.header.confirmedTick,
                    .sourceScriptId = effect.header.sourceScript.value,
                    .sourceEffectOrdinal = effect.header.ordinal,
                    .kind = ObjectOrderKind::CommandButton,
                    .actors = std::move(actors),
                    .targetObject = effectiveTargetObject.value_or(
                        INVALID_OBJECT_ID),
                    .targetPosition = targetPosition,
                    .contentName = payload.buttonName,
                }, payload.actorSelector ==
                       ScriptOrderActorSelector::NamedObjects);
            if (!result.accepted) {
                bridge.emitDiagnostic(effect.header,
                    "Script CommandButton was rejected: " + result.message);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptOrderEffect>) {
            container::Vector<ObjectId> actors;
            actors.reserve(payload.actors.size());
            bool effectiveAllArmyHunt = payload.allArmyHunt;
            ScriptOrderAuthority authority = ScriptOrderAuthority::NamedObjects;
            ObjectTeamId scenarioTeam = INVALID_OBJECT_TEAM_ID;
            if (payload.actorSelector == ScriptOrderActorSelector::ScenarioTeam) {
                const std::optional<ObjectTeamId> resolvedTeam = payload.scenarioTeam
                    ? std::optional<ObjectTeamId>{payload.scenarioTeam}
                    : bridge.resolveEffectTeam(payload.teamName, effect.header);
                // RefCode's getTeamNamed() failure is a no-op. Do not turn a
                // missing optional map team into a malformed command or an
                // accidental player-default-team expansion.
                if (!resolvedTeam || !bridge.objectTeams().find(*resolvedTeam)) return;
                authority = ScriptOrderAuthority::ScenarioTeam;
                scenarioTeam = *resolvedTeam;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*resolvedTeam);
                actors.assign(members.begin(), members.end());
            } else if (payload.actorSelector ==
                       ScriptOrderActorSelector::PlayerAssets) {
                const std::optional<PlayerId> player = bridge.resolvePlayer(
                    payload.playerName,
                    effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!player || !bridge.players().get(*player)) return;
                static_cast<void>(
                    bridge.players().setUnitsShouldHunt(*player, true));

                // Player::setUnitsShouldHunt addresses current live units,
                // skips economy/select-all exclusions, and silently ignores
                // objects without AI. Intersect the stable ownership index
                // with the ObjectAI admission capability before submitting one atomic
                // script order so an ineligible structure cannot reject the
                // eligible army around it.
                for (const ObjectId object : bridge.ownership().objects(*player)) {
                    if (!bridge.objectAIRuntime().hasOrderCapability(
                            object, ai::ObjectAIOrderCapability::Attack)) {
                        continue;
                    }
                    const std::optional<ecs::entity> entity =
                        bridge.entityFromId(object);
                    const ThingTemplateComponent* type = entity
                        ? ecs::try_get<ThingTemplateComponent>(
                              bridge.registry(), *entity)
                        : nullptr;
                    if (!type || !type->archetype ||
                        kindOfContains(type->archetype,
                                       game::ObjectKindOf::Dozer) ||
                        kindOfContains(type->archetype,
                                       game::ObjectKindOf::Harvester) ||
                        kindOfContains(type->archetype,
                                       game::ObjectKindOf::IgnoresSelectAll)) {
                        continue;
                    }
                    actors.push_back(object);
                }
            } else {
                actors = payload.actors;
            }

            if (!effectiveAllArmyHunt &&
                payload.kind == ScriptOrderKind::TacticalAttack &&
                payload.tacticalAttackSubtype ==
                    ScriptTacticalAttackSubtype::Hunt &&
                !actors.empty()) {
                const std::optional<ecs::entity> entity =
                    bridge.entityFromId(actors.front());
                const OwnerComponent* owner = entity
                    ? ecs::try_get<OwnerComponent>(
                          bridge.registry(), *entity)
                    : nullptr;
                const PlayerState* player = owner
                    ? bridge.players().get(owner->player)
                    : nullptr;
                effectiveAllArmyHunt = player && player->unitsShouldHunt;
            }

            const auto disbandScenarioTeam = [&]() {
                const ObjectTeamRecord* source =
                    bridge.objectTeams().find(scenarioTeam);
                const std::optional<ObjectTeamId> destination = source
                    ? bridge.objectTeams().defaultTeam(source->owner)
                    : std::nullopt;
                if (!source || !destination) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(scenarioTeam);
                container::Vector<ObjectId> snapshot{
                    members.begin(), members.end()};
                for (const ObjectId object : snapshot) {
                    if (bridge.m_queries.sequentialObjectState(object).hasAI) {
                        static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                            object, ObjectPanelFlag::AiRecruitable, true,
                            effect.header.confirmedTick));
                    }
                    if (*destination != scenarioTeam) {
                        static_cast<void>(
                            bridge.m_ownershipTransactions.transferObjectToTeam(
                                object, *destination,
                                effect.header.confirmedTick));
                    }
                }
                if (*destination == scenarioTeam) return;
                static_cast<void>(bridge.setTeamActive(
                    scenarioTeam, false, effect.header.confirmedTick));
                static_cast<void>(bridge.setTeamActive(
                    *destination, true, effect.header.confirmedTick));
            };

            // An empty Player or Team army is a normal legacy no-op, not a
            // malformed zero-actor order. Stop+Disband is the exception:
            // RefCode still deletes the empty source Team.
            if (actors.empty()) {
                if (payload.disbandAfterStop) disbandScenarioTeam();
                return;
            }

            const ObjectMoveRouteSubtype moveRouteSubtype =
                toObjectMoveRouteSubtype(payload.moveRouteSubtype);
            const bool adaptiveWanderPanicPath =
                moveRouteSubtype == ObjectMoveRouteSubtype::WanderWaypointPath ||
                moveRouteSubtype == ObjectMoveRouteSubtype::PanicWaypointPath;
            uint32_t waypointStartId =
                std::numeric_limits<uint32_t>::max();
            uint64_t waypointGraphRevision = 0;
            if (isObjectWaypointRouteSubtype(moveRouteSubtype)) {
                const bool asTeam =
                    objectWaypointRouteMovesAsTeam(moveRouteSubtype);
                if (payload.kind != ScriptOrderKind::Move ||
                    (asTeam && authority !=
                        ScriptOrderAuthority::ScenarioTeam) ||
                    (!asTeam && authority ==
                         ScriptOrderAuthority::NamedObjects &&
                     actors.size() != 1) ||
                    payload.targetWaypointName.empty() ||
                    payload.targetPosition || payload.targetObject ||
                    payload.queued) {
                    bridge.emitDiagnostic(
                        effect.header,
                        "Rejected malformed waypoint-path order effect");
                    return;
                }
                if (asTeam) {
                    // AIGroup dispatches waypoint orders per member. Build a
                    // deterministic live-consumer set before choosing the
                    // path anchor so an incapable member cannot veto or
                    // anchor the rest of the Team.
                    actors = bridge.m_orderAdmissionTransactions.selectScriptMoveOrderActors(
                        container::Span<const ObjectId>{actors}, authority);
                    if (actors.empty()) return;
                }
                if (adaptiveWanderPanicPath) {
                    // RefCode resolves the nearest waypoint separately for
                    // each live Team member. Delay that lookup until the
                    // per-actor dispatch below.
                    waypointGraphRevision =
                        bridge.terrain().waypointGraphRevision();
                } else {
                    // RefCode chooses a Team waypoint path from the Team's
                    // center, not from whichever member happens to sort
                    // first. Named-object orders contain exactly one actor,
                    // so the same calculation also covers them without a
                    // separate path.
                    if (actors.empty()) return;
                    size_t centerCount = 0;
                    for (const ObjectId actorId : actors) {
                        const std::optional<ecs::entity> actor =
                            bridge.entityFromId(actorId);
                        if (actor && ecs::try_get<TransformComponent>(
                                bridge.registry(), *actor)) {
                            ++centerCount;
                        }
                    }
                    if (centerCount == 0) return;
                    const int64_t divisor =
                        static_cast<int64_t>(centerCount);
                    math::FixedRawMeanAccumulator centerX{divisor};
                    math::FixedRawMeanAccumulator centerY{divisor};
                    for (const ObjectId actorId : actors) {
                        const std::optional<ecs::entity> actor =
                            bridge.entityFromId(actorId);
                        const TransformComponent* transform = actor
                            ? ecs::try_get<TransformComponent>(
                                  bridge.registry(), *actor)
                            : nullptr;
                        if (!transform) continue;
                        const LogicFixedVec3 position =
                            readAuthoritativeObjectPosition(
                                bridge.registry(), *actor,
                                *transform);
                        centerX.add(position.x.raw());
                        centerY.add(position.y.raw());
                    }
                    const game::terrain::WaypointRecord* waypoint =
                        bridge.terrain().closestWaypointOnPathRaw(
                            centerX.value(), centerY.value(),
                            payload.targetWaypointName);
                    // Missing units/AI/path labels are legacy script no-ops; no
                    // targetless Move may reach the queue.
                    if (!waypoint) return;
                    waypointStartId = waypoint->id;
                    waypointGraphRevision =
                        bridge.terrain().waypointGraphRevision();
                }
            }

            CommandPosition targetPosition;
            if (moveRouteSubtype == ObjectMoveRouteSubtype::Direct &&
                payload.targetPosition) {
                targetPosition = {
                    .x = payload.targetPosition->x,
                    .y = payload.targetPosition->y,
                    .z = payload.targetPosition->z,
                    .valid = true,
                };
            } else if (moveRouteSubtype == ObjectMoveRouteSubtype::Direct &&
                       !payload.targetWaypointName.empty()) {
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().waypointByName(payload.targetWaypointName);
                // The legacy action simply returns when a map lacks the
                // referenced waypoint. This is a legitimate script no-op,
                // not a failed player command.
                if (!waypoint) return;
                targetPosition = {
                    .x = math::q32_32::from_raw(waypoint->positionRaw[0]),
                    .y = math::q32_32::from_raw(waypoint->positionRaw[1]),
                    .z = math::q32_32::from_raw(waypoint->positionRaw[2]),
                    .valid = true,
                };
            }
            ObjectTeamId tacticalTargetTeam = INVALID_OBJECT_TEAM_ID;
            uint32_t tacticalTargetAreaId =
                std::numeric_limits<uint32_t>::max();
            uint64_t tacticalTargetRevision = 0;
            if (payload.tacticalAttackSubtype ==
                ScriptTacticalAttackSubtype::AttackSquad) {
                const std::optional<ObjectTeamId> targetTeam =
                    bridge.resolveEffectTeam(
                        payload.targetTeamName, effect.header);
                if (!targetTeam ||
                    !bridge.objectTeams().find(*targetTeam)) return;
                tacticalTargetTeam = *targetTeam;
                tacticalTargetRevision =
                    bridge.objectTeams().membershipRevision(*targetTeam);
            } else if (payload.tacticalAttackSubtype ==
                           ScriptTacticalAttackSubtype::AttackArea ||
                       (payload.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::Guard &&
                        !payload.targetAreaName.empty())) {
                const game::terrain::PolygonTriggerRecord* area =
                    bridge.terrain().triggerByName(
                        payload.targetAreaName);
                if (!area) return;
                tacticalTargetAreaId = area->id;
                tacticalTargetRevision =
                    game::terrain::TerrainLogic::triggerRevision(*area);
                if (payload.tacticalAttackSubtype ==
                    ScriptTacticalAttackSubtype::Guard) {
                    const auto bounds =
                        game::terrain::TerrainLogic::legacyTriggerBounds(*area);
                    if (!bounds || bounds->radius < math::q32_32{}) {
                        return;
                    }
                    targetPosition = {
                        .x = bounds->centerX,
                        .y = bounds->centerY,
                        .z = math::q32_32::from_raw(
                            bridge.terrain().groundHeightRaw(
                                bounds->centerX.raw(),
                                bounds->centerY.raw())),
                        .valid = true,
                    };
                }
            }
            if (adaptiveWanderPanicPath) {
                for (const ObjectId actorId : actors) {
                    const std::optional<ecs::entity> actor =
                        bridge.entityFromId(actorId);
                    const TransformComponent* transform = actor
                        ? ecs::try_get<TransformComponent>(
                              bridge.registry(), *actor)
                        : nullptr;
                    if (!transform) continue;
                    const LogicFixedVec3 position =
                        readAuthoritativeObjectPosition(
                            bridge.registry(), *actor,
                            *transform);
                    const game::terrain::WaypointRecord* waypoint =
                        bridge.terrain().closestWaypointOnPathRaw(
                            position.x.raw(), position.y.raw(),
                            payload.targetWaypointName);
                    // RefCode returns from the Team loop at the first missing
                    // path, preserving orders already issued to earlier
                    // members in legacy intrusive-list order.
                    if (!waypoint) return;
                    container::Vector<ObjectId> singleActor{actorId};
                    const OrderExecutionResult actorResult =
                        bridge.m_orderAdmissionTransactions.executeScriptOrder({
                            .contextPlayer =
                                effect.header.invocation.currentPlayer,
                            .authority = authority,
                            .scenarioTeam = scenarioTeam,
                            .confirmedTick = effect.header.confirmedTick,
                            .sourceScriptId = effect.header.sourceScript.value,
                            .sourceEffectOrdinal = effect.header.ordinal,
                            .kind = ObjectOrderKind::Move,
                            .moveRouteSubtype = moveRouteSubtype,
                            .actors = std::move(singleActor),
                            .waypointStartId = waypoint->id,
                            .waypointGraphRevision = waypointGraphRevision,
                        });
                    if (!actorResult.accepted) {
                        bridge.emitDiagnostic(
                            effect.header,
                            "Script wander/panic order was rejected: " +
                                actorResult.message);
                    }
                }
                return;
            }
            const OrderExecutionResult result = bridge.m_orderAdmissionTransactions.executeScriptOrder({
                .contextPlayer = effect.header.invocation.currentPlayer,
                .authority = authority,
                .scenarioTeam = scenarioTeam,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .sourceEffectOrdinal = effect.header.ordinal,
                .kind = toObjectOrderKind(payload.kind),
                .tacticalAttackSubtype =
                    toObjectTacticalAttackSubtype(payload.tacticalAttackSubtype),
                .moveRouteSubtype = moveRouteSubtype,
                .actors = std::move(actors),
                .targetObject = payload.targetObject.value_or(INVALID_OBJECT_ID),
                .targetPosition = targetPosition,
                .contentName = payload.contentName,
                .forceAttack = payload.forceAttack,
                .waypointStartId = waypointStartId,
                .waypointGraphRevision = waypointGraphRevision,
                .allArmyHunt = effectiveAllArmyHunt,
                .useTeamCommonTarget = payload.useTeamCommonTarget,
                .tacticalTargetTeam = tacticalTargetTeam,
                .tacticalTargetAreaId = tacticalTargetAreaId,
                .tacticalTargetRevision = tacticalTargetRevision,
                .queued = payload.queued,
            });
            if (!result.accepted) {
                bridge.emitDiagnostic(effect.header, "Script order was rejected: " + result.message);
            } else if (payload.disbandAfterStop) {
                // TEAM_STOP_AND_DISBAND executes groupIdle first, then marks
                // AI members recruitable and merges the live Scenario Team
                // into its controlling player's default Team. Do this only
                // after Stop admission succeeds so no partial disband can
                // survive a malformed order boundary.
                disbandScenarioTeam();
            }
        } else if constexpr (std::is_same_v<Payload, ScriptAIBehaviorMutationEffect>) {
            if (payload.targetKind ==
                ScriptAIBehaviorTargetKind::NamedObject) {
                switch (payload.mutation) {
                case ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet:
                    static_cast<void>(bridge.applyAttackPrioritySet(
                        payload.object, payload.attackPrioritySet));
                    break;
                case ScriptAIBehaviorMutationKind::SetAttitude:
                    if (payload.attitude >= -2 && payload.attitude <= 2 &&
                        bridge.objectAIRuntime().actorState(payload.object)) {
                        static_cast<void>(
                            bridge.m_objectTransactions.setAIAttitude(
                                payload.object,
                                static_cast<ObjectAIAttitude>(
                                    payload.attitude)));
                    }
                    break;
                case ScriptAIBehaviorMutationKind::SetCommandButtonHunt:
                    break;
                case ScriptAIBehaviorMutationKind::IncreaseTeamProductionPriority:
                case ScriptAIBehaviorMutationKind::DecreaseTeamProductionPriority:
                case ScriptAIBehaviorMutationKind::WanderInPlace:
                    // These actions require a ScenarioTeam target; a named
                    // object payload is malformed and remains a no-op.
                    break;
                }
                return;
            }
            const std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) return;
            if (payload.mutation ==
                ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet) {
                static_cast<void>(bridge.applyTeamAttackPrioritySet(
                    *team, payload.attackPrioritySet));
                return;
            }
            if (payload.mutation ==
                ScriptAIBehaviorMutationKind::SetCommandButtonHunt) {
                static_cast<void>(bridge.setTeamCommandButtonHunt(
                    *team, payload.commandButton,
                    effect.header.confirmedTick));
                return;
            }
            if (payload.mutation ==
                ScriptAIBehaviorMutationKind::IncreaseTeamProductionPriority ||
                payload.mutation ==
                    ScriptAIBehaviorMutationKind::DecreaseTeamProductionPriority) {
                const int32_t delta = payload.mutation ==
                    ScriptAIBehaviorMutationKind::IncreaseTeamProductionPriority
                    ? 1 : -1;
                static_cast<void>(
                    bridge.objectTeams().adjustProductionPriority(
                        *team, delta));
                return;
            }
            if (payload.mutation == ScriptAIBehaviorMutationKind::WanderInPlace) {
                static_cast<void>(bridge.setTeamWanderInPlace(
                    *team, effect.header.confirmedTick));
                return;
            }
            const container::Span<const ObjectId> members =
                bridge.objectTeams().legacyMembers(*team);
            for (const ObjectId member : members) {
                if (payload.attitude < -2 || payload.attitude > 2 ||
                    !bridge.objectAIRuntime().actorState(member)) {
                    continue;
                }
                static_cast<void>(bridge.m_objectTransactions.setAIAttitude(
                    member,
                    static_cast<ObjectAIAttitude>(payload.attitude)));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptAttackPriorityMutationEffect>) {
            static_cast<void>(bridge.mutateAttackPrioritySet(
                payload.mutation, payload.setName, payload.selectors,
                payload.priority));
        } else if constexpr (std::is_same_v<Payload, ScriptFacingEffect>) {
            container::Vector<ObjectId> actors = payload.actors;
            if (payload.actorSelector ==
                ScriptOrderActorSelector::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!team) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                actors.assign(members.begin(), members.end());
            }
            if (actors.empty()) return;

            std::optional<LogicFixedVec3> targetPosition;
            if (!payload.targetObject) {
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().waypointByName(
                        payload.targetWaypointName);
                if (!waypoint) return;
                targetPosition = LogicFixedVec3{
                    math::q32_32::from_raw(waypoint->positionRaw[0]),
                    math::q32_32::from_raw(waypoint->positionRaw[1]),
                    math::q32_32::from_raw(waypoint->positionRaw[2]),
                };
            }
            for (const ObjectId actor : actors) {
                static_cast<void>(bridge.m_orderTransactions.face(
                    actor,
                    payload.targetObject.value_or(INVALID_OBJECT_ID),
                    targetPosition, effect.header.confirmedTick));
            }
        }
        }
    }, effect.payload);
    return handled;
}

} // namespace detail
} // namespace engine::script

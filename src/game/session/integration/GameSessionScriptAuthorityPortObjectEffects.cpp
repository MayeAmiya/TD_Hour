#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptAuthorityPort.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

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
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] uint32_t legacyFloatRoundedFrameCount(
    math::q32_32 seconds, uint32_t framesPerSecond) noexcept {
    const math::q32_32 scaled = seconds * math::q32_32{
        static_cast<int32_t>(framesPerSecond)};
    const int64_t scaledRaw = scaled.raw();
    if (scaledRaw <= 0) return 0;

    uint64_t magnitude = static_cast<uint64_t>(scaledRaw);
    const unsigned width = std::bit_width(magnitude);
    const unsigned discardedBits = width > 24u ? width - 24u : 0u;
    if (discardedBits != 0u) {
        const uint64_t quotient = magnitude >> discardedBits;
        const uint64_t mask = (uint64_t{1} << discardedBits) - 1u;
        const uint64_t remainder = magnitude & mask;
        const uint64_t halfway = uint64_t{1} << (discardedBits - 1u);
        uint64_t rounded = quotient;
        if (remainder > halfway ||
            (remainder == halfway && (quotient & 1u) != 0u)) {
            ++rounded;
        }
        magnitude = rounded >
                (std::numeric_limits<uint64_t>::max() >> discardedBits)
            ? std::numeric_limits<uint64_t>::max()
            : rounded << discardedBits;
    }
    const uint64_t frames = magnitude >> 32u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        frames, static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
}

} // namespace

bool GameSessionScriptAuthorityPort::setHulkLifetimeOverride(
    math::q32_32 seconds) noexcept {
    if (!m_content.m_active) return false;
    if (seconds < math::q32_32{}) {
        m_world.m_objectSimulation.setHulkLifetimeOverrideFrames(std::nullopt);
        return true;
    }
    const uint32_t framesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    m_world.m_objectSimulation.setHulkLifetimeOverrideFrames(
        legacyFloatRoundedFrameCount(seconds, framesPerSecond));
    return true;
}

bool GameSessionScriptAuthorityPort::setToppleDirection(
    container::StringView objectName, LogicFixedVec3 direction) {
    if (objectName.empty()) return false;
    const container::String key{objectName};
    const auto found = m_presentation.m_scriptToppleDirections.find(key);
    const bool changed =
        found == m_presentation.m_scriptToppleDirections.end() ||
        found->second.x != direction.x ||
        found->second.y != direction.y ||
        found->second.z != direction.z;
    m_presentation.m_scriptToppleDirections.insert_or_assign(key, direction);

    const std::optional<ObjectId> object =
        m_presentation.m_scriptObjects.liveNamedObject(objectName);
    const std::optional<ecs::entity> entity = object
        ? m_world.m_objects.entityFromId(*object)
        : std::nullopt;
    if (!entity) return changed;

    static_cast<void>(m_objectTransactions.setScriptToppleDirection(
        *object, direction));
    return changed;
}

using detail::kindOfContains;

namespace detail {

bool applyObjectEffect(
    GameSessionScriptAuthorityPort& bridge, const ScriptEffect& effect) {
    bool handled = false;
    std::visit([&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, ScriptMapPresentationEffect>) {
            const bool authorityCommand =
                payload.command != ScriptMapPresentationCommand::CreateRadarEvent &&
                payload.command != ScriptMapPresentationCommand::SetBorderShroud &&
                payload.command != ScriptMapPresentationCommand::SetRadarHidden &&
                payload.command != ScriptMapPresentationCommand::SetRadarForced;
            if (authorityCommand) {
                handled = true;
                if (!bridge.applyMapAuthority(
                        payload, effect.header.confirmedTick,
                        effect.header.sourceScript.value, effect.header.ordinal,
                        effect.header.invocation.currentPlayer,
                        effect.header.currentPlayerAlias)) {
                    bridge.emitDiagnostic(effect.header,
                        "Script map-authority effect was rejected");
                }
            }
        }
        if constexpr (std::is_same_v<Payload, ScriptWaterEffect>) {
            handled = bridge.applyWaterAuthority(payload, effect.header);
        }
        if constexpr (std::is_same_v<Payload, ScriptTeamCustomStateEffect> ||
                      std::is_same_v<Payload, ScriptSpecialPowerCountdownEffect> ||
                      std::is_same_v<Payload, ScriptWarehouseValueEffect> ||
                      std::is_same_v<Payload, ScriptCaveIndexEffect> ||
                      std::is_same_v<Payload, ScriptStoppingDistanceEffect> ||
                      std::is_same_v<Payload, ScriptMoveTowardsNearestEffect> ||
                      std::is_same_v<Payload, ScriptCreateObjectEffect> ||
                      std::is_same_v<Payload, ScriptDestroyObjectEffect> ||
                      std::is_same_v<Payload, ScriptLifecycleEffect> ||
                      std::is_same_v<Payload, ScriptTimeControlEffect> ||
                      std::is_same_v<Payload, ScriptScoreAccumulationPolicyEffect> ||
                      std::is_same_v<Payload, ScriptHulkLifetimeOverrideEffect> ||
                      std::is_same_v<Payload, ScriptContainmentEffect> ||
                      std::is_same_v<Payload, ScriptContainmentEnterEffect> ||
                      std::is_same_v<Payload, ScriptTransferOwnershipEffect> ||
                      std::is_same_v<Payload, ScriptDamageEffect> ||
                      std::is_same_v<Payload, ScriptGrantObjectUpgradeEffect> ||
                      std::is_same_v<Payload, ScriptObjectStateMutationEffect> ||
                      std::is_same_v<Payload, ScriptGlobalObjectEffect> ||
                      std::is_same_v<Payload, ScriptBoobyTrapEffect> ||
                      std::is_same_v<Payload, ScriptToppleDirectionEffect>) {
            handled = true;
        if constexpr (std::is_same_v<Payload, ScriptTeamCustomStateEffect>) {
            if (payload.team) {
                static_cast<void>(bridge.objectTeams().setScriptState(
                    payload.team, payload.state));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSpecialPowerCountdownEffect>) {
            const SpecialPowerDefinition* definition =
                bridge.contentSnapshot().findSpecialPower(payload.specialPower);
            if (definition) {
                static_cast<void>(
                    bridge.m_objectTransactions.mutateSpecialPowerCountdown(
                        payload.object, *definition, payload.operation,
                        payload.seconds, payload.paused,
                        bridge.startInfo().gameSpeedFPS,
                        effect.header.confirmedTick));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptWarehouseValueEffect>) {
            static_cast<void>(bridge.m_objectTransactions.setWarehouseCashValue(
                payload.object, payload.cashValue));
        } else if constexpr (std::is_same_v<Payload, ScriptCaveIndexEffect>) {
            static_cast<void>(bridge.m_objectTransactions.setCaveIndex(
                payload.object, payload.caveIndex));
        } else if constexpr (std::is_same_v<Payload, ScriptStoppingDistanceEffect>) {
            container::Vector<ObjectId> targets;
            if (payload.targetKind == ScriptStoppingDistanceTargetKind::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!team) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                targets.assign(members.begin(), members.end());
            } else if (payload.object) {
                targets.push_back(payload.object);
            }
            for (const ObjectId target : targets) {
                const bool applied = bridge.m_objectTransactions.setStoppingDistance(
                    target, payload.distance);
                // RefCode aborts the Team loop at the first member without an
                // AI locomotor. With the legacy sidecar this preserves both
                // the newest-first prefix and the original early return.
                if (!applied && payload.targetKind ==
                        ScriptStoppingDistanceTargetKind::ScenarioTeam) {
                    return;
                }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptMoveTowardsNearestEffect>) {
            container::Vector<ObjectId> actors = payload.actors;
            ScriptOrderAuthority authority = ScriptOrderAuthority::NamedObjects;
            ObjectTeamId scenarioTeam = INVALID_OBJECT_TEAM_ID;
            if (payload.actorSelector == ScriptOrderActorSelector::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!team) return;
                authority = ScriptOrderAuthority::ScenarioTeam;
                scenarioTeam = *team;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                actors.assign(members.begin(), members.end());
            }
            if (actors.empty() || payload.objectTypes.empty() ||
                !bridge.terrain().triggerByName(payload.triggerArea)) return;

            math::q32_32 centerX{};
            math::q32_32 centerY{};
            size_t centerCount = 0;
            bool referenceOffMap = false;
            bool haveReferenceMapStatus = false;
            if (payload.actorSelector == ScriptOrderActorSelector::ScenarioTeam) {
                // RefCode does not calculate a centroid here. Its misleadingly
                // named getEstimateTeamPosition() returns the prepended list
                // head, while the map-status filter uses the first member with
                // AI (or the last live member if none expose AI).
                const std::optional<ecs::entity> first =
                    bridge.entityFromId(actors.front());
                const TransformComponent* firstTransform = first
                    ? ecs::try_get<TransformComponent>(
                          bridge.registry(), *first)
                    : nullptr;
                if (!firstTransform) return;
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        bridge.registry(), *first,
                        *firstTransform);
                centerX = position.x;
                centerY = position.y;
                centerCount = 1;
                for (const ObjectId actor : actors) {
                    const std::optional<ecs::entity> entity =
                        bridge.entityFromId(actor);
                    if (!entity) continue;
                    const ObjectMapStatusComponent* status =
                        ecs::try_get<ObjectMapStatusComponent>(bridge.registry(), *entity);
                    referenceOffMap = status && status->offMap;
                    haveReferenceMapStatus = true;
                    if (bridge.m_queries.sequentialObjectState(actor).hasAI) break;
                }
            } else {
                for (const ObjectId actor : actors) {
                    const std::optional<ecs::entity> entity =
                        bridge.entityFromId(actor);
                    const TransformComponent* transform = entity
                        ? ecs::try_get<TransformComponent>(
                              bridge.registry(), *entity)
                        : nullptr;
                    if (!entity || !transform) continue;
                    const LogicFixedVec3 position =
                        readAuthoritativeObjectPosition(
                            bridge.registry(), *entity,
                            *transform);
                    centerX += position.x;
                    centerY += position.y;
                    ++centerCount;
                    if (!haveReferenceMapStatus) {
                        const ObjectMapStatusComponent* status =
                            ecs::try_get<ObjectMapStatusComponent>(
                                bridge.registry(), *entity);
                        referenceOffMap = status && status->offMap;
                        haveReferenceMapStatus = true;
                    }
                }
            }
            if (centerCount == 0) return;
            if (centerCount > static_cast<size_t>(
                    std::numeric_limits<int32_t>::max())) return;
            const math::q32_32 fixedCenterCount{
                static_cast<int32_t>(centerCount)};
            centerX /= fixedCenterCount;
            centerY /= fixedCenterCount;

            const game::terrain::PolygonTriggerRecord* trigger =
                bridge.terrain().triggerByName(payload.triggerArea);
            if (!trigger) return;

            ObjectId best = INVALID_OBJECT_ID;
            math::q32_32 bestDistance{};
            const auto candidates = ecs::view<const ObjectIdentityComponent,
                                              const TransformComponent,
                                              const ThingTemplateComponent>(
                bridge.registry());
            for (const ecs::entity entity : candidates) {
                const ObjectIdentityComponent& identity =
                    candidates.template get<const ObjectIdentityComponent>(entity);
                const TransformComponent& transform =
                    candidates.template get<const TransformComponent>(entity);
                const ThingTemplateComponent& type =
                    candidates.template get<const ThingTemplateComponent>(entity);
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        bridge.registry(), entity, transform);
                if (!identity.id || !bridge.entityFromId(identity.id) ||
                    ecs::try_get<ObjectContainedByComponent>(
                        bridge.registry(), entity) ||
                    !type.archetype ||
                    !bridge.terrain().isInsideTriggerLegacyRaw(
                        *trigger, position.x.raw(), position.y.raw())) continue;
                const ObjectMapStatusComponent* status =
                    ecs::try_get<ObjectMapStatusComponent>(bridge.registry(), entity);
                if ((status && status->offMap) != referenceOffMap) continue;
                const container::StringView typeName = type.archetype->templateData.name;
                const bool matches = std::any_of(
                    payload.objectTypes.begin(), payload.objectTypes.end(),
                    [typeName](const container::String& wanted) {
                        return equalAsciiInsensitive(typeName, wanted);
                    });
                if (!matches) continue;
                const math::q32_32 dx = position.x - centerX;
                const math::q32_32 dy = position.y - centerY;
                const math::q32_32 distance = dx * dx + dy * dy;
                if (!best || distance < bestDistance ||
                    (distance == bestDistance && identity.id < best)) {
                    best = identity.id;
                    bestDistance = distance;
                }
            }
            if (!best) return;
            static_cast<void>(bridge.m_orderAdmissionTransactions.executeScriptOrder({
                .contextPlayer = effect.header.invocation.currentPlayer,
                .authority = authority,
                .scenarioTeam = scenarioTeam,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .sourceEffectOrdinal = effect.header.ordinal,
                .kind = ObjectOrderKind::Move,
                .actors = std::move(actors),
                .targetObject = best,
            }));
        } else if constexpr (std::is_same_v<Payload, ScriptCreateObjectEffect>) {
            const bool hasPosition = payload.position.has_value();
            const bool hasWaypoint = !payload.waypointName.empty();
            if (payload.templateName.empty() || payload.teamName.empty() ||
                hasPosition == hasWaypoint) {
                bridge.emitDiagnostic(effect.header, "Rejected malformed Script object-create effect");
                return;
            }

            // ScriptActions::doCreateObject checks a live named Object before
            // it calls getTeamNamed()/createTeam. This must precede lazy Team
            // materialization: a rejected duplicate name is not a Team
            // creation event. GameSession repeats the same predicate at the
            // allocation boundary, where it also owns name transfer.
            if (!payload.objectName.empty() &&
                !bridge.canCreateNamedObject(payload.objectName)) {
                bridge.emitDiagnostic(effect.header,
                    "Script object-create effect was rejected for live object name: " +
                    payload.objectName);
                return;
            }

            // RefCode first asks ScriptEngine::getTeamNamed(), then invokes
            // TeamFactory::createTeam only for a known prototype with no
            // usable instance. Keep that materialization/activation decision
            // session-owned so a malformed alias cannot be redirected to a
            // player-default Team or created by a read-only query.
            std::optional<ObjectTeamId> team =
                bridge.resolveEffectTeam(payload.teamName, effect.header);
            if (!team) {
                team = bridge.ensureScenarioTeamForCreate(
                    payload.teamName, effect.header.confirmedTick);
            }
            if (!team) {
                bridge.emitDiagnostic(effect.header,
                    "Script object-create effect referenced an unresolved Scenario Team: " +
                    payload.teamName);
                return;
            }
            const std::optional<PlayerId> owner = bridge.objectTeams().teamOwner(*team);
            if (!owner || !bridge.players().get(*owner)) {
                bridge.emitDiagnostic(effect.header,
                    "Script object-create effect resolved a Team without a live owner: " +
                    payload.teamName);
                return;
            }

            LogicFixedVec3 position{};
            if (payload.position) {
                // doCreateObject receives a world Coord3D directly. Unlike a
                // startup Map Object record, it must not acquire terrain
                // ground height a second time.
                position = {
                    payload.position->x,
                    payload.position->y,
                    payload.position->z,
                };
            } else if (const game::terrain::WaypointRecord* waypoint =
                           bridge.terrain().waypointByName(payload.waypointName)) {
                position = {
                    math::q32_32::from_raw(waypoint->positionRaw[0]),
                    math::q32_32::from_raw(waypoint->positionRaw[1]),
                    math::q32_32::from_raw(waypoint->positionRaw[2]),
                };
            }
            // RefCode's createUnitOnTeamAt creates first and only then checks
            // the waypoint. A missing waypoint therefore intentionally leaves
            // this default transform at world origin instead of cancelling the
            // object creation.

            ObjectSpawnRequest request;
            request.templateName = payload.templateName;
            request.owner = *owner;
            request.primaryTeam = *team;
            request.transform = ObjectFixedTransformComponent{
                .position = position,
                .yawRadians = payload.rotation,
                .authoritative = true,
            };
            request.origin = ObjectCreationOrigin::Script;
            request.confirmedTick = effect.header.confirmedTick;
            request.scriptName = payload.objectName;
            // GameSession repeats the preflight atomically with allocation and
            // owns the eventual transfer/binding. The bridge never mutates
            // ScriptObjectIndex directly.
            request.replaceEffectivelyDeadScriptName = !payload.objectName.empty();

            GameSessionObjectSpawnResult created =
                bridge.m_lifecycleTransactions.spawnObject(std::move(request));
            if (!created) {
                bridge.emitDiagnostic(effect.header,
                    "Script object-create effect was rejected for template/name: " +
                    payload.templateName);
                return;
            }

            if (!payload.objectName.empty() && !created.scriptNameBound) {
                // A script-requested name is atomic in the creation path;
                // reaching this branch means GameSession detected an internal
                // invariant violation and already discarded the entity.
                bridge.emitDiagnostic(effect.header,
                    "Script object-create effect could not bind requested object name: " +
                    payload.objectName);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptDestroyObjectEffect>) {
            // RefCode's doNamedDelete is a silent no-op for an unavailable
            // object.  A preceding ScriptDamageEffect may also have made the
            // object PendingDestroy at this exact boundary, so it is not a
            // bridge diagnostic.
            if (payload.object) {
                if (payload.forceKill) {
                    static_cast<void>(bridge.m_damageTransactions.queueObjectDamage({
                        .target = payload.object,
                        .source = INVALID_OBJECT_ID,
                        .sourceSequence = effect.header.ordinal,
                        .amount = math::q32_32{},
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = effect.header.confirmedTick,
                    }));
                    bridge.m_damageTransactions.resolveQueuedObjectDamage();
                } else {
                    static_cast<void>(
                        bridge.m_lifecycleTransactions.destroyObject(
                            payload.object));
                }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptLifecycleEffect>) {
            container::Vector<ObjectId> targets;
            std::optional<ObjectTeamId> targetTeam;
            std::optional<PlayerId> targetPlayer;
            if (payload.targetKind == ScriptLifecycleTargetKind::ScenarioTeam) {
                targetTeam = bridge.resolveEffectTeam(payload.targetName, effect.header);
                if (!targetTeam) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*targetTeam);
                targets.assign(members.begin(), members.end());
            } else {
                targetPlayer = bridge.resolvePlayer(
                    payload.targetName, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!targetPlayer || !bridge.players().get(*targetPlayer)) return;
                const container::Span<const ObjectId> owned =
                    bridge.ownership().objects(*targetPlayer);
                targets.assign(owned.begin(), owned.end());
            }

            // Legacy Team/Player kill first evacuates live containers. A
            // player-default Team delete does the same before structural
            // removal because garrison ownership changes mutate membership.
            // The central containment journal receives a stable member
            // snapshot, so detaches cannot invalidate this traversal.
            const ObjectTeamRecord* teamRecord = targetTeam
                ? bridge.objectTeams().find(*targetTeam) : nullptr;
            const bool evacuate = payload.operation == ScriptLifecycleOperation::Kill ||
                (teamRecord && teamRecord->kind == ObjectTeamKind::PlayerDefault);
            if (evacuate) {
                for (const ObjectId target : targets) {
                    static_cast<void>(bridge.m_containmentTransactions.request({
                        .kind = ObjectContainmentRequestKind::EjectAll,
                        .container = target,
                        .confirmedTick = effect.header.confirmedTick,
                    }));
                }
            }

            for (const ObjectId target : targets) {
                const std::optional<ecs::entity> entity = bridge.entityFromId(target);
                const ObjectHealthComponent* health = entity
                    ? ecs::try_get<ObjectHealthComponent>(bridge.registry(), *entity)
                    : nullptr;
                if (payload.operation == ScriptLifecycleOperation::DeleteLiving &&
                    health && health->effectivelyDead) {
                    continue;
                }
                if (payload.operation == ScriptLifecycleOperation::Kill) {
                    const ObjectTechBuildingComponent* tech = entity
                        ? ecs::try_get<ObjectTechBuildingComponent>(
                              bridge.registry(), *entity)
                        : nullptr;
                    if (tech && !tech->beacons.empty()) {
                        static_cast<void>(
                            bridge.m_lifecycleTransactions.destroyObject(
                                target));
                        continue;
                    }
                    if (health && health->effectivelyDead) continue;
                    const ThingTemplateComponent* type = entity
                        ? ecs::try_get<ThingTemplateComponent>(bridge.registry(), *entity)
                        : nullptr;
                    if (type && type->archetype && kindOfContains(
                            type->archetype, game::ObjectKindOf::TechBuilding)) {
                        const std::optional<ObjectTeamId> neutralTeam =
                            bridge.objectTeams().defaultTeam(NEUTRAL_PLAYER_ID);
                        if (neutralTeam) {
                            static_cast<void>(
                                bridge.m_ownershipTransactions.transferObjectToTeam(
                                    target, *neutralTeam,
                                    effect.header.confirmedTick));
                        }
                        continue;
                    }
                    static_cast<void>(bridge.m_damageTransactions.queueObjectDamage({
                        .target = target,
                        .source = INVALID_OBJECT_ID,
                        .sourceSequence = effect.header.ordinal,
                        .amount = math::q32_32{},
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = effect.header.confirmedTick,
                    }));
                } else {
                    static_cast<void>(
                        bridge.m_lifecycleTransactions.destroyObject(target));
                }
            }
            if (payload.operation == ScriptLifecycleOperation::Kill) {
                bridge.m_damageTransactions.resolveQueuedObjectDamage();
                if (targetPlayer) {
                    const PlayerState* state = bridge.players().get(*targetPlayer);
                    const bool preserveSinglePlayerAi = state &&
                        state->controller == PlayerControllerKind::Ai &&
                        bridge.startInfo().mode == GameMode::SinglePlayer;
                    static_cast<void>(bridge.m_playerTransactions.setLifeState(
                        *targetPlayer, preserveSinglePlayerAi
                            ? PlayerLifeState::Active : PlayerLifeState::Defeated));
                    if (!preserveSinglePlayerAi) {
                        static_cast<void>(bridge.m_playerTransactions.setCash(
                            *targetPlayer, 0));
                    }
                }
            } else if (targetTeam) {
                static_cast<void>(bridge.setTeamActive(
                    *targetTeam, false, effect.header.confirmedTick));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptTimeControlEffect>) {
            bridge.setTimeFrozen(payload.frozen);
        } else if constexpr (std::is_same_v<Payload, ScriptScoreAccumulationPolicyEffect>) {
            bridge.setScoreAccumulationEnabled(payload.enabled);
        } else if constexpr (std::is_same_v<Payload, ScriptHulkLifetimeOverrideEffect>) {
            if (!bridge.setHulkLifetimeOverride(payload.seconds)) {
                bridge.emitDiagnostic(effect.header,
                    "Script hulk-lifetime override effect was rejected by authority");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptContainmentEffect>) {
            container::Vector<ObjectId> targets;
            switch (payload.kind) {
            case ScriptContainmentActionKind::EjectContainerContents:
            case ScriptContainmentActionKind::EjectSpecificStructure:
            case ScriptContainmentActionKind::DetachNamedOccupant:
            case ScriptContainmentActionKind::KillContainerContents:
            case ScriptContainmentActionKind::SetEvacuationDisposition:
                if (payload.namedTarget) targets.push_back(payload.namedTarget);
                break;
            case ScriptContainmentActionKind::EjectTeamContainerContents:
            case ScriptContainmentActionKind::DetachTeamOccupants: {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.targetName, effect.header);
                if (!team) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                targets.assign(members.begin(), members.end());
                break;
            }
            case ScriptContainmentActionKind::EjectPlayerStructures: {
                const std::optional<PlayerId> player = bridge.resolvePlayer(
                    payload.targetName, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!player) return;
                const container::Span<const ObjectId> owned =
                    bridge.ownership().objects(*player);
                for (const ObjectId object : owned) {
                    const std::optional<ecs::entity> entity = bridge.entityFromId(object);
                    const ThingTemplateComponent* type = entity
                        ? ecs::try_get<ThingTemplateComponent>(bridge.registry(), *entity)
                        : nullptr;
                    if (type && type->archetype && kindOfContains(
                            type->archetype, game::ObjectKindOf::Structure)) {
                        targets.push_back(object);
                    }
                }
                break;
            }
            }

            if (payload.kind ==
                    ScriptContainmentActionKind::SetEvacuationDisposition) {
                if (targets.empty()) return;
                const auto disposition = payload.evacuationDisposition == 1
                    ? ObjectContainmentEvacuationDisposition::Left
                    : payload.evacuationDisposition == 2
                        ? ObjectContainmentEvacuationDisposition::Right
                        : payload.evacuationDisposition == 3
                            ? ObjectContainmentEvacuationDisposition::
                                  BurstFromCenter
                            : ObjectContainmentEvacuationDisposition::Invalid;
                static_cast<void>(
                    bridge.m_containmentTransactions.setEvacuationDisposition(
                        targets.front(), disposition));
                return;
            }

            if (payload.kind == ScriptContainmentActionKind::EjectSpecificStructure) {
                if (targets.empty()) return;
                const std::optional<ecs::entity> entity =
                    bridge.entityFromId(targets.front());
                const ThingTemplateComponent* type = entity
                    ? ecs::try_get<ThingTemplateComponent>(bridge.registry(), *entity)
                    : nullptr;
                if (!type || !type->archetype || !kindOfContains(
                        type->archetype, game::ObjectKindOf::Structure)) return;
            }

            if (payload.kind == ScriptContainmentActionKind::KillContainerContents) {
                if (!targets.empty()) {
                    static_cast<void>(
                        bridge.m_containmentPlanTransactions.killContainedObjects(
                            targets.front(), effect.header.ordinal,
                            effect.header.confirmedTick));
                }
                return;
            }
            const bool detach =
                payload.kind == ScriptContainmentActionKind::DetachNamedOccupant ||
                payload.kind == ScriptContainmentActionKind::DetachTeamOccupants;
            for (const ObjectId target : targets) {
                static_cast<void>(bridge.m_containmentTransactions.request({
                    .kind = detach ? ObjectContainmentRequestKind::Detach
                                   : ObjectContainmentRequestKind::EjectAll,
                    .container = detach ? INVALID_OBJECT_ID : target,
                    .object = detach ? target : INVALID_OBJECT_ID,
                    .confirmedTick = effect.header.confirmedTick,
                }));
            }
        } else if constexpr (std::is_same_v<Payload,
                                                   ScriptContainmentEnterEffect>) {
            const auto resolveObjectId = [&](const ScriptObjectSelector& selector)
                -> std::optional<ObjectId> {
                const std::optional<ScriptWorldObjectSnapshot> object =
                    bridge.m_queries.resolveObjectSelector(
                        selector, effect.header.invocation);
                return object && object->alive && object->id
                    ? std::optional<ObjectId>{object->id}
                    : std::nullopt;
            };
            const auto resolveTeamId = [&]() -> std::optional<ObjectTeamId> {
                const std::optional<ObjectTeamId> team = bridge.m_queries.resolveTeamSelector(
                    payload.team, effect.header.invocation);
                return team && bridge.objectTeams().find(*team)
                    ? team : std::nullopt;
            };
            switch (payload.kind) {
            case ScriptContainmentEnterActionKind::LoadTeamTransports: {
                const std::optional<ObjectTeamId> team = resolveTeamId();
                if (team) {
                    static_cast<void>(
                        bridge.m_containmentPlanTransactions.requestTeamLoadTransports(
                            *team, effect.header.ordinal,
                            effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::TeamCaptureNearestUnmanned: {
                const std::optional<ObjectTeamId> team = resolveTeamId();
                if (team) {
                    static_cast<void>(
                        bridge.m_containmentPlanTransactions
                            .requestTeamCaptureNearestUnmanned(
                                *team, effect.header.ordinal,
                                effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::NamedEnterNamed: {
                const std::optional<ObjectId> object =
                    resolveObjectId(payload.object);
                const std::optional<ObjectId> container =
                    resolveObjectId(payload.container);
                if (object && container) {
                    static_cast<void>(
                        bridge.m_containmentTransactions.requestObjectEnter(
                        *object, *container, effect.header.ordinal,
                        effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::TeamEnterNamed:
            case ScriptContainmentEnterActionKind::TeamGarrisonSpecific: {
                const std::optional<ObjectTeamId> team = resolveTeamId();
                const std::optional<ObjectId> container =
                    resolveObjectId(payload.container);
                if (team && container) {
                    static_cast<void>(
                        bridge.m_containmentTransactions.requestTeamEnter(
                        *team, *container,
                        payload.kind ==
                            ScriptContainmentEnterActionKind::
                                TeamGarrisonSpecific,
                        effect.header.ordinal,
                        effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::TeamGarrisonNearest: {
                const std::optional<ObjectTeamId> team = resolveTeamId();
                if (team) {
                    static_cast<void>(
                        bridge.m_containmentPlanTransactions
                            .requestTeamGarrisonNearest(
                                *team, effect.header.ordinal,
                                effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::NamedGarrisonSpecific:
            case ScriptContainmentEnterActionKind::NamedGarrisonNearest: {
                const std::optional<ObjectId> object =
                    resolveObjectId(payload.object);
                const std::optional<ObjectId> building =
                    payload.kind == ScriptContainmentEnterActionKind::
                                        NamedGarrisonSpecific
                    ? resolveObjectId(payload.container)
                    : std::optional<ObjectId>{};
                if (object &&
                    (payload.kind == ScriptContainmentEnterActionKind::
                                         NamedGarrisonNearest || building)) {
                    static_cast<void>(
                        bridge.m_containmentTransactions.requestObjectGarrison(
                        *object, building, effect.header.ordinal,
                        effect.header.confirmedTick));
                }
                break;
            }
            case ScriptContainmentEnterActionKind::PlayerGarrisonAll: {
                const std::optional<PlayerId> player = bridge.resolvePlayer(
                    payload.player, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (player) {
                    static_cast<void>(
                        bridge.m_containmentPlanTransactions
                            .requestPlayerGarrisonAll(
                                *player, effect.header.ordinal,
                                effect.header.confirmedTick));
                }
                break;
            }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptTransferOwnershipEffect>) {
            switch (payload.selector) {
            case ScriptOwnershipTransferSelector::NamedObject: {
                const std::optional<PlayerId> target = bridge.resolvePlayer(
                    payload.targetPlayer, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!target || !bridge.players().get(*target)) return;
                if (!payload.object) return;
                // ScriptActions moves a named Object to the destination
                // Player's default Team even when it already has that Player
                // as owner. `changeObjectOwner` intentionally short-circuits
                // the latter modern case, so use the more precise Team
                // transaction here.
                const std::optional<ObjectTeamId> defaultTeam =
                    bridge.objectTeams().defaultTeam(*target);
                if (defaultTeam) {
                    static_cast<void>(
                        bridge.m_ownershipTransactions.transferObjectToTeam(
                            payload.object, *defaultTeam,
                            effect.header.confirmedTick));
                }
                return;
            }
            case ScriptOwnershipTransferSelector::ScenarioTeam: {
                const std::optional<PlayerId> target = bridge.resolvePlayer(
                    payload.targetPlayer, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!target || !bridge.players().get(*target)) return;
                // Legacy getTeamNamed() only observes an existing live Team;
                // ownership transfer must never lazily create a prototype.
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (team) {
                    static_cast<void>(
                        bridge.m_ownershipTransactions.transferTeamOwnership(
                            *team, *target, effect.header.confirmedTick));
                }
                return;
            }
            case ScriptOwnershipTransferSelector::PlayerAssets: {
                const std::optional<PlayerId> source = bridge.resolvePlayer(
                    payload.sourcePlayer, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                const std::optional<PlayerId> target = bridge.resolvePlayer(
                    payload.targetPlayer, effect.header.invocation.currentPlayer,
                    effect.header.currentPlayerAlias);
                if (!source || !target || *source == *target) return;
                const PlayerState* sourceState = bridge.players().get(*source);
                const PlayerState* targetState = bridge.players().get(*target);
                const std::optional<ObjectTeamId> destination =
                    bridge.objectTeams().defaultTeam(*target);
                if (!sourceState || !targetState || !destination) return;
                const int64_t transferredCash = sourceState->cash;
                const container::Span<const ObjectId> sourceObjects =
                    bridge.ownership().objects(*source);
                container::Vector<ObjectId> snapshot{
                    sourceObjects.begin(), sourceObjects.end()};
                for (const ObjectId object : snapshot) {
                    const std::optional<ecs::entity> entity =
                        bridge.entityFromId(object);
                    const ObjectTechBuildingComponent* tech = entity
                        ? ecs::try_get<ObjectTechBuildingComponent>(
                              bridge.registry(), *entity)
                        : nullptr;
                    // Player::transferAssetsFromThat deliberately retains
                    // the source faction's multiplayer beacon.
                    if (tech && !tech->beacons.empty()) continue;
                    static_cast<void>(
                        bridge.m_ownershipTransactions.transferObjectToTeam(
                            object, *destination, effect.header.confirmedTick));
                }
                static_cast<void>(bridge.m_playerTransactions.setCash(
                    *source, 0));
                static_cast<void>(bridge.m_playerTransactions.adjustCash(
                    *target, transferredCash));
                return;
            }
            case ScriptOwnershipTransferSelector::MergeScenarioTeam: {
                const std::optional<ObjectTeamId> source =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!source) return;
                std::optional<ObjectTeamId> target =
                    bridge.resolveEffectTeam(payload.targetTeamName, effect.header);
                if (!target) {
                    target = bridge.ensureScenarioTeamForCreate(
                        payload.targetTeamName, effect.header.confirmedTick);
                }
                if (!target || *target == *source) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*source);
                container::Vector<ObjectId> snapshot{members.begin(), members.end()};
                for (const ObjectId object : snapshot) {
                    static_cast<void>(
                        bridge.m_ownershipTransactions.transferObjectToTeam(
                            object, *target, effect.header.confirmedTick));
                }
                static_cast<void>(bridge.setTeamActive(
                    *source, false, effect.header.confirmedTick));
                static_cast<void>(bridge.setTeamActive(
                    *target, true, effect.header.confirmedTick));
                return;
            }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptDamageEffect>) {
            container::Vector<ObjectId> targets;
            if (payload.targetSelector == ScriptDamageTargetSelector::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                // RefCode's getTeamNamed() failure is a normal no-op.  Expand
                // the live membership only at the stamped application point,
                // after any earlier delete/transfer effect has committed.
                if (!team) return;
                const container::Span<const ObjectId> members = bridge.objectTeams().members(*team);
                targets.assign(members.begin(), members.end());
            } else {
                targets = payload.targets;
            }

            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            for (const ObjectId target : targets) {
                if (!target) continue;
                // Damage transactions are the sole Body/Health ingress.  The
                // emit() drains this stamped batch before the next effect,
                // preserving the authored (ordinal, target) sequence without
                // turning a scenario action into a network GameCommand.
                static_cast<void>(bridge.m_damageTransactions.queueObjectDamage({
                    .target = target,
                    .source = INVALID_OBJECT_ID,
                    .sourceSequence = effect.header.ordinal,
                    .amount = payload.amount,
                    .damageType = game::DamageType::UNRESISTABLE,
                    .deathType = game::DeathType::NORMAL,
                    .forceKill = payload.forceKill,
                    .confirmedTick = effect.header.confirmedTick,
                }));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptGrantObjectUpgradeEffect>) {
            // RefCode silently ignores a missing object or unknown Upgrade.
            // The runtime already resolved liveness; GameSession performs the
            // frozen UpgradeCatalog/type/affected-object transaction here.
            if (payload.object && !payload.upgradeName.empty()) {
                static_cast<void>(
                    bridge.m_progressionTransactions.completeObjectUpgrade(
                        payload.object, payload.upgradeName));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptObjectStateMutationEffect>) {
            container::Vector<ObjectId> targets;
            if (payload.targetKind == ScriptObjectStateTargetKind::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                // getTeamNamed() failure is a normal no-op. Membership is
                // deliberately expanded here, after all earlier stamped
                // effects, rather than frozen in ScriptRuntime.
                if (!team) return;
                if (payload.mutation ==
                    ScriptObjectStateMutationKind::TeamRecruitable) {
                    static_cast<void>(
                        bridge.objectTeams().setRecruitableOverride(
                            *team, payload.enabled));
                    return;
                }
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                targets.assign(members.begin(), members.end());
            } else if (payload.object) {
                targets.push_back(payload.object);
            }

            for (const ObjectId target : targets) {
                switch (payload.mutation) {
                case ScriptObjectStateMutationKind::Held:
                    static_cast<void>(bridge.m_objectTransactions.setHeld(
                        target, payload.enabled, effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::Repulsor:
                    static_cast<void>(bridge.m_objectTransactions.setRepulsor(
                        target, payload.enabled, effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::Unmanned:
                    if (const std::optional<ObjectTeamId> neutralTeam =
                            bridge.objectTeams().defaultTeam(
                                NEUTRAL_PLAYER_ID)) {
                        static_cast<void>(
                            bridge.m_objectTransactions.setUnmanned(
                                target, effect.header.confirmedTick));
                        static_cast<void>(
                            bridge.m_ownershipTransactions.transferObjectToTeam(
                                target, *neutralTeam,
                                effect.header.confirmedTick));
                    }
                    break;
                case ScriptObjectStateMutationKind::RailroadHeld:
                    static_cast<void>(bridge.m_objectTransactions.setRailroadHeld(
                        target, payload.enabled));
                    break;
                case ScriptObjectStateMutationKind::StealthEnabled:
                    static_cast<void>(bridge.m_objectTransactions.setStealthEnabled(
                        target, payload.enabled, effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelEnabled:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::Enabled, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelPowered:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::Powered, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelIndestructible:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::Indestructible, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelUnsellable:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::Unsellable, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelSelectable:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::Selectable, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelAiRecruitable:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::AiRecruitable, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::PanelPlayerTargetable:
                    static_cast<void>(bridge.m_objectTransactions.setPanelFlag(
                        target, ObjectPanelFlag::PlayerTargetable, payload.enabled,
                        effect.header.confirmedTick));
                    break;
                case ScriptObjectStateMutationKind::TeamRecruitable:
                    // Consumed above as a Team-instance policy; it must never
                    // be expanded into per-object panel state.
                    break;
                }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptGlobalObjectEffect>) {
            if (payload.operation == ScriptGlobalObjectOperation::DeleteAllUnmanned) {
                static_cast<void>(bridge.destroyAllUnmanned(
                    effect.header.confirmedTick));
                return;
            }
            const bool idle =
                payload.operation == ScriptGlobalObjectOperation::IdleHumanUnits;
            for (const PlayerId player : bridge.players().activePlayerIds()) {
                const PlayerState* state = bridge.players().get(player);
                if (!state || state->controller != PlayerControllerKind::Human) continue;
                container::Vector<ObjectId> actors;
                const container::Span<const ObjectId> owned =
                    bridge.ownership().objects(player);
                for (const ObjectId object : owned) {
                    const std::optional<ecs::entity> entity = bridge.entityFromId(object);
                    const ThingTemplateComponent* type = entity
                        ? ecs::try_get<ThingTemplateComponent>(bridge.registry(), *entity)
                        : nullptr;
                    if (type && type->archetype && kindOfContains(
                            type->archetype, game::ObjectKindOf::Structure)) continue;
                    if (idle) actors.push_back(object);
                    static_cast<void>(bridge.m_objectTransactions.setSupplyTruckIdleSuppressed(
                        object, idle, effect.header.confirmedTick));
                }
                if (idle && !actors.empty()) {
                    static_cast<void>(bridge.m_orderAdmissionTransactions.executeScriptOrder({
                        .contextPlayer = player,
                        .authority = ScriptOrderAuthority::NamedObjects,
                        .confirmedTick = effect.header.confirmedTick,
                        .sourceScriptId = effect.header.sourceScript.value,
                        .sourceEffectOrdinal = effect.header.ordinal,
                        .kind = ObjectOrderKind::Stop,
                        .actors = std::move(actors),
                    }));
                }
            }
        } else if constexpr (std::is_same_v<Payload, ScriptBoobyTrapEffect>) {
            container::Vector<ObjectId> targets;
            if (payload.targetKind == ScriptObjectStateTargetKind::ScenarioTeam) {
                const std::optional<ObjectTeamId> team =
                    bridge.resolveEffectTeam(payload.teamName, effect.header);
                if (!team) return;
                const container::Span<const ObjectId> members =
                    bridge.objectTeams().legacyMembers(*team);
                targets.assign(members.begin(), members.end());
            } else if (payload.object) {
                targets.push_back(payload.object);
            }
            for (const ObjectId target : targets) {
                static_cast<void>(
                    bridge.m_progressionTransactions.attachScriptBoobyTrap(
                        target, payload.templateName,
                        effect.header.confirmedTick));
            }
        } else if constexpr (std::is_same_v<Payload, ScriptToppleDirectionEffect>) {
            static_cast<void>(bridge.setToppleDirection(
                payload.objectName,
                {payload.direction.x, payload.direction.y,
                 payload.direction.z}));
        }
        }
    }, effect.payload);
    return handled;
}

} // namespace detail
} // namespace engine::script

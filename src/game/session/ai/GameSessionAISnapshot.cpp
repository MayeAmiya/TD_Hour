#include "game/session/core/GameSession.h"
#include "game/session/ai/GameSessionAIDomain.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/scenario/runtime/ScenarioDefinition.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

#include "game/object/ai/runtime/ObjectAIStableDigest.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"

namespace engine {
namespace {

[[nodiscard]] std::optional<uint32_t> terrainWaypointId(
    ai::AIWaypointHandle handle) noexcept {
    if (!handle || handle.value - 1 >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(handle.value - 1);
}

[[nodiscard]] ai::AIWaypointQuery queryTerrainWaypointNode(
    const void* context, ai::AIWaypointHandle handle,
    uint64_t revision) noexcept {
    const auto* terrain =
        static_cast<const game::terrain::TerrainLogic*>(context);
    if (!terrain) return {};
    if (revision != terrain->waypointGraphRevision()) {
        return {.status = ai::AIWaypointQueryStatus::StaleRevision};
    }
    const std::optional<uint32_t> id = terrainWaypointId(handle);
    const game::terrain::WaypointRecord* waypoint = id
        ? terrain->waypointById(*id) : nullptr;
    if (!waypoint) {
        return {.status = ai::AIWaypointQueryStatus::Missing};
    }
    return {
        .status = ai::AIWaypointQueryStatus::Node,
        .node = {
            .position = {
                .xRaw = waypoint->positionRaw[0],
                .yRaw = waypoint->positionRaw[1],
                .zRaw = waypoint->positionRaw[2],
            },
            .linkCount = static_cast<uint32_t>(waypoint->links.size()),
            // RefCode uses a five-node lookahead only as a pathfinder
            // optimization. Zero preserves route correctness without making
            // the resolver depend on Navigation's mutable cell size.
            .lookAheadDistanceRaw = 0,
            .wall = false,
        },
    };
}

[[nodiscard]] ai::AIWaypointLinkQuery queryTerrainWaypointLink(
    const void* context, ai::AIWaypointHandle handle,
    uint64_t revision, uint32_t index) noexcept {
    const auto* terrain =
        static_cast<const game::terrain::TerrainLogic*>(context);
    if (!terrain) return {};
    if (revision != terrain->waypointGraphRevision()) {
        return {.status = ai::AIWaypointQueryStatus::StaleRevision};
    }
    const std::optional<uint32_t> id = terrainWaypointId(handle);
    const game::terrain::WaypointRecord* waypoint = id
        ? terrain->waypointById(*id) : nullptr;
    if (!waypoint || index >= waypoint->links.size() ||
        !terrain->waypointById(waypoint->links[index])) {
        return {.status = ai::AIWaypointQueryStatus::Missing};
    }
    return {
        .status = ai::AIWaypointQueryStatus::Node,
        .target = ai::AIWaypointHandle{
            static_cast<uint64_t>(waypoint->links[index]) + 1},
    };
}

[[nodiscard]] ai::AIWaypointGraphResolver terrainWaypointResolver(
    const game::terrain::TerrainLogic& terrain) noexcept {
    return {
        .context = &terrain,
        .queryNode = &queryTerrainWaypointNode,
        .queryLink = &queryTerrainWaypointLink,
    };
}

[[nodiscard]] bool appendValidatedObjectAIPathSequence(
    ObjectId object,
    const std::optional<ObjectOrderQueueComponent>& queueValue,
    const std::optional<ObjectSystemPathSequenceComponent>& routeValue,
    ai::ObjectAIPathSequenceSnapshot& output,
    container::Vector<ai::AIFixedPosition>& scratch) {
    if (!routeValue) return true;
    if (!queueValue || queueValue->orders.empty()) return false;

    const ObjectSystemPathSequenceComponent& route = *routeValue;
    const ObjectOrderQueueComponent& queue = *queueValue;
    const bool playerPath =
        route.routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
        route.source == ObjectOrderSource::Player &&
        route.systemPurpose == ObjectOrderSystemPurpose::Generic;
    const bool supported = playerPath ||
        (route.routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
         route.source == ObjectOrderSource::System &&
         route.systemPurpose ==
             ObjectOrderSystemPurpose::ContainmentExit) ||
        (route.routeSubtype ==
             ObjectMoveRouteSubtype::FollowExitProductionPath &&
         route.source == ObjectOrderSource::System &&
         route.systemPurpose ==
             ObjectOrderSystemPurpose::ProductionExit);
    bool valid = supported && route.queuedOrderCount != 0 &&
        route.queuedOrderCount == route.points.size() &&
        route.queuedOrderCount <= queue.orders.size();
    uint32_t expectedSequence = route.firstSourceSequence;
    for (uint32_t index = 0; valid && index < route.queuedOrderCount;
         ++index) {
        const ObjectOrderIntent& segment = queue.orders[index];
        const LogicFixedVec3 target{
            segment.targetX, segment.targetY, segment.targetZ};
        valid = segment.kind == ObjectOrderKind::Move &&
            segment.source == route.source &&
            segment.systemPurpose == route.systemPurpose &&
            segment.moveRouteSubtype ==
                (index == 0 ? route.routeSubtype
                            : ObjectMoveRouteSubtype::Direct) &&
            (playerPath ||
             (segment.issuedTick == route.issuedTick &&
              segment.sourceSequence == expectedSequence)) &&
            segment.hasTargetPosition &&
            target.x == route.points[index].x &&
            target.y == route.points[index].y &&
            target.z == route.points[index].z;
        if (expectedSequence != std::numeric_limits<uint32_t>::max())
            ++expectedSequence;
    }
    if (valid && playerPath) {
        valid = queue.orders.front().issuedTick == route.issuedTick &&
            queue.orders.front().sourceSequence ==
                route.firstSourceSequence;
    }
    if (!valid) return false;

    scratch.clear();
    scratch.reserve(route.points.size());
    for (const LogicFixedVec3& point : route.points) {
        scratch.push_back({
            .xRaw = point.x.raw(),
            .yRaw = point.y.raw(),
            .zRaw = point.z.raw(),
        });
    }
    return output.append(
        ai::AIPathSequenceHandle{static_cast<uint64_t>(object.value)},
        scratch, nullptr, route.sequenceRevision);
}

} // namespace

namespace {

[[nodiscard]] container::Vector<ObjectId> objectAIWorldOrderSubjects(
    const ai::ObjectAIRuntime& runtime, const ecs::registry& registry) {
    container::Vector<ObjectId> result;
    const auto economyView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectEconomyComponent>(registry);
    const auto locomotionView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectLocomotionComponent>(registry);
    const auto evacuationView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectPendingPlayerEvacuationComponent>(registry);
    const auto teamView = ecs::view<
        const ObjectIdentityComponent,
        const PrimaryTeamComponent>(registry);
    result.reserve(runtime.activeCount() + economyView.size_hint() +
                   locomotionView.size_hint() + evacuationView.size_hint() +
                   teamView.size_hint());
    for (const ai::AIStateSoASubjectSlot& actor : runtime.orderedSubjects()) {
        const std::optional<ai::ObjectAIOrderCapability> capabilities =
            runtime.orderCapabilities(actor.subject);
        if (capabilities &&
            (ai::hasObjectAIOrderCapability(
                 *capabilities, ai::ObjectAIOrderCapability::MoveStop) ||
             ai::hasObjectAIOrderCapability(
                 *capabilities, ai::ObjectAIOrderCapability::Attack))) {
            result.push_back(actor.subject);
        }
    }
    for (const ecs::entity entity : economyView) {
        const ObjectId object = economyView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (object) result.push_back(object);
    }
    // Specialized locomotor owners (Chinook/Transport/RailedTransport) do not
    // appear in ObjectAIRuntime's generic move-owner lanes, but their queues,
    // precise-Z state and player evacuation continuation are still part of
    // the authoritative movement slice captured here.
    for (const ecs::entity entity : locomotionView) {
        const ObjectId object = locomotionView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (object) result.push_back(object);
    }
    for (const ecs::entity entity : evacuationView) {
        const ObjectId object = evacuationView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (object) result.push_back(object);
    }
    for (const ecs::entity entity : teamView) {
        const ObjectId object = teamView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (object) result.push_back(object);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] bool captureLocomotionRuntime(
    const ObjectLocomotionComponent& source,
    ObjectAILocomotionRuntimeSnapshot& output) {
    if (!source.fixedRuntimeInitialized || source.profiles.empty() ||
        source.templateName.empty()) {
        return false;
    }

    ObjectAILocomotionRuntimeSnapshot candidate;
    candidate.profileNames.reserve(source.profiles.size());
    bool activeProfileFound = false;
    for (const game::FrozenLocomotorTemplate& profile : source.profiles) {
        if (profile.name.empty() ||
            std::find(candidate.profileNames.begin(),
                      candidate.profileNames.end(), profile.name) !=
                candidate.profileNames.end()) {
            return false;
        }
        candidate.profileNames.push_back(profile.name);
        activeProfileFound = activeProfileFound ||
            profile.name == source.templateName;
    }
    if (!activeProfileFound) return false;

    candidate.activeProfileName = source.templateName;
    candidate.closeEnough = source.closeEnough;
    candidate.forwardSpeed = source.forwardSpeed;
    candidate.verticalSpeed = source.verticalSpeed;
    candidate.groundOffset = source.groundOffsetFixed;
    candidate.goal = source.goal;
    candidate.activeOrderTick = source.activeOrderTick;
    candidate.activeOrderSequence = source.activeOrderSequence;
    candidate.activeSourceScriptId = source.activeSourceScriptId;
    candidate.usePreciseZPosition = source.usePreciseZPosition;
    candidate.ultraAccurate = source.ultraAccurate;
    candidate.overWater = source.overWater;
    candidate.hasActiveMove = source.hasActiveMove;
    candidate.movingBackward = source.movingBackward;
    candidate.state = source.state;
    output = std::move(candidate);
    return true;
}

[[nodiscard]] bool stageLocomotionRestore(
    const ObjectAILocomotionRuntimeSnapshot& snapshot,
    const ObjectLocomotionComponent& current,
    const GameContentSnapshot& content,
    ObjectLocomotionComponent& output) {
    if (snapshot.profileNames.empty() ||
        snapshot.activeProfileName.empty() ||
        static_cast<uint8_t>(snapshot.state) >
            static_cast<uint8_t>(ObjectLocomotionState::Blocked)) {
        return false;
    }

    ObjectLocomotionComponent candidate = current;
    candidate.profiles.clear();
    candidate.profiles.reserve(snapshot.profileNames.size());
    const game::FrozenLocomotorTemplate* activeProfile = nullptr;
    for (size_t index = 0; index < snapshot.profileNames.size(); ++index) {
        const container::String& name = snapshot.profileNames[index];
        if (name.empty() ||
            std::find(snapshot.profileNames.begin(),
                      snapshot.profileNames.begin() +
                          static_cast<std::ptrdiff_t>(index),
                      name) != snapshot.profileNames.begin() +
                          static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
        const game::FrozenLocomotorTemplate* profile = content.findLocomotor(name);
        if (!profile || !profile->supportsRuntimeLocomotion()) return false;
        candidate.profiles.push_back(*profile);
        if (profile->name == snapshot.activeProfileName)
            activeProfile = &candidate.profiles.back();
    }
    if (!activeProfile) return false;

    object_simulation_detail::applyLocomotorTemplate(candidate,
                                                      *activeProfile);
    candidate.closeEnough = snapshot.closeEnough;
    candidate.forwardSpeed = snapshot.forwardSpeed;
    candidate.verticalSpeed = snapshot.verticalSpeed;
    candidate.groundOffsetFixed = snapshot.groundOffset;
    candidate.goal = snapshot.goal;
    candidate.activeOrderTick = snapshot.activeOrderTick;
    candidate.activeOrderSequence = snapshot.activeOrderSequence;
    candidate.activeSourceScriptId = snapshot.activeSourceScriptId;
    candidate.usePreciseZPosition = snapshot.usePreciseZPosition;
    candidate.ultraAccurate = snapshot.ultraAccurate;
    candidate.overWater = snapshot.overWater;
    candidate.hasActiveMove = snapshot.hasActiveMove;
    candidate.movingBackward = snapshot.movingBackward;
    candidate.state = snapshot.state;
    candidate.fixedRuntimeInitialized = true;
    output = std::move(candidate);
    return true;
}

} // namespace

ObjectAIWorldSnapshotStatus GameSessionAIDomain::captureObjectAIWorldSnapshot(
    ObjectAIWorldSnapshot& output) const {
    if (!domainState().contentState().m_active)
        return ObjectAIWorldSnapshotStatus::SessionInactive;
    if (domainState().contentState().m_drainingGameplayWork)
        return ObjectAIWorldSnapshotStatus::Busy;

    ObjectAIWorldSnapshot candidate;
    candidate.confirmedTick = domainState().presentationState().m_confirmedTick;
    candidate.hasConfirmedFrame = domainState().presentationState().m_hasConfirmedFrame;
    if ((!candidate.hasConfirmedFrame && candidate.confirmedTick != 0) ||
        (candidate.hasConfirmedFrame && candidate.confirmedTick == 0)) {
        return ObjectAIWorldSnapshotStatus::InvalidTickState;
    }
    if (domainState().aiState().m_objectAI.captureSnapshot(candidate.aiRuntime) !=
            ai::ObjectAIRuntimeSnapshotStatus::Success) {
        return ObjectAIWorldSnapshotStatus::AIRuntimeRejected;
    }
    if (!domainState().aiState().m_strategicAI.captureSnapshot(
            candidate.strategicAI)) {
        return ObjectAIWorldSnapshotStatus::StrategicAIRejected;
    }
    if (!domainState().worldState().m_objectTeams.captureSnapshot(
            candidate.objectTeams)) {
        return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
    }
    for (const ObjectTeamRecord& team :
         domainState().worldState().m_objectTeams.teams()) {
        if (!team.id) continue;
        for (const ObjectId member : team.members.values()) {
            const std::optional<ecs::entity> entity =
                domainState().worldState().m_objects.entityFromId(member);
            const PrimaryTeamComponent* primaryTeam = entity
                ? ecs::try_get<PrimaryTeamComponent>(
                      domainState().worldState().m_registry, *entity)
                : nullptr;
            const OwnerComponent* owner = entity
                ? ecs::try_get<OwnerComponent>(
                      domainState().worldState().m_registry, *entity)
                : nullptr;
            if (!primaryTeam || primaryTeam->team != team.id || !owner ||
                owner->player != team.owner) {
                return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
            }
            const ObjectRelationshipOverrideComponent* relationship =
                ecs::try_get<ObjectRelationshipOverrideComponent>(
                    domainState().worldState().m_registry, *entity);
            if (team.relationshipPolicy
                    ? !relationship ||
                          relationship->policy != team.relationshipPolicy
                    : relationship != nullptr) {
                return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
            }
        }
    }
    for (const ecs::entity entity :
         ecs::view<const PrimaryTeamComponent>(
             domainState().worldState().m_registry)) {
        const ObjectIdentityComponent* identity =
            ecs::try_get<const ObjectIdentityComponent>(
                domainState().worldState().m_registry, entity);
        const PrimaryTeamComponent& primaryTeam =
            ecs::get<const PrimaryTeamComponent>(
                domainState().worldState().m_registry, entity);
        if (!identity || !identity->id)
            return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
        // DestroyRequested removes the stable Team index immediately while
        // the ECS component remains until physical retirement. It is absent
        // from the restorable live-object slice by design.
        if (domainState().worldState().m_objects.isPendingDestroy(identity->id))
            continue;
        if (domainState().worldState().m_objectTeams.teamOf(identity->id) !=
                std::optional<ObjectTeamId>{primaryTeam.team}) {
            return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
        }
    }
    for (const ai::ObjectAIRecipeBindingSnapshot& binding :
         domainState().aiState().m_objectAI.recipeBindings()) {
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(binding.subject);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(
                  domainState().worldState().m_registry, *entity)
            : nullptr;
        const ai::AIRecipeId recipe =
            type && type->archetype && type->archetype->hasAiUpdate
            ? type->archetype->aiRecipe
            : ai::AIRecipeId::Invalid;
        const bool matches = binding.state ==
                ai::ObjectAIRecipeBindingState::Bound
            ? recipe != ai::AIRecipeId::Invalid && recipe == binding.recipe
            : binding.state ==
                  ai::ObjectAIRecipeBindingState::ContentUnavailable &&
                  recipe == ai::AIRecipeId::Invalid;
        if (!matches)
            return ObjectAIWorldSnapshotStatus::RecipeMismatch;
    }
    if (domainState().contentState().m_navigation.captureSnapshot(candidate.navigation) !=
            navigation::NavigationSystemStatus::Success) {
        return ObjectAIWorldSnapshotStatus::NavigationRejected;
    }
    const container::Vector<ObjectId> owners =
        objectAIWorldOrderSubjects(domainState().aiState().m_objectAI, domainState().worldState().m_registry);
    candidate.orderOwners.reserve(owners.size());
    ai::ObjectAIPathSequenceSnapshot pathSequenceValidation;
    container::Vector<ai::AIFixedPosition> pathSequenceScratch;
    ObjectId previousOwner = INVALID_OBJECT_ID;
    for (const ObjectId object : owners) {
        if (!object || (previousOwner && !(previousOwner < object)))
            return ObjectAIWorldSnapshotStatus::InvalidObjectOrder;
        previousOwner = object;

        ObjectAIWorldOrderOwnerSnapshot record;
        record.object = object;
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(object);
        record.objectPresent = entity.has_value();
        if (entity) {
            if (const ObjectFixedTransformComponent* value =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        domainState().worldState().m_registry, *entity)) {
                if (!value->authoritative) {
                    return ObjectAIWorldSnapshotStatus::
                        ComponentPresenceMismatch;
                }
                record.fixedTransform = *value;
            }
            if (const ObjectLocomotionComponent* value =
                    ecs::try_get<ObjectLocomotionComponent>(
                        domainState().worldState().m_registry, *entity)) {
                ObjectAILocomotionRuntimeSnapshot locomotion;
                if (!captureLocomotionRuntime(*value, locomotion)) {
                    return ObjectAIWorldSnapshotStatus::
                        ComponentPresenceMismatch;
                }
                record.locomotion = std::move(locomotion);
            }
            if (const ObjectAIPathMovementComponent* value =
                    ecs::try_get<ObjectAIPathMovementComponent>(
                        domainState().worldState().m_registry, *entity))
                record.pathMovement = *value;
            if (const ObjectAIMovementObstructionStateComponent* value =
                    ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                        domainState().worldState().m_registry, *entity))
                record.movementObstruction = *value;
            if (const ObjectTemporaryCollisionIgnoreComponent* value =
                    ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                        domainState().worldState().m_registry, *entity))
                record.temporaryCollisionIgnore = *value;
            if (const ObjectRepulsorExpiryComponent* value =
                    ecs::try_get<ObjectRepulsorExpiryComponent>(
                        domainState().worldState().m_registry, *entity))
                record.repulsorExpiry = *value;
            if (const ObjectWaypointCompletionComponent* value =
                    ecs::try_get<ObjectWaypointCompletionComponent>(
                        domainState().worldState().m_registry, *entity))
                record.waypointCompletion = *value;
            if (const ObjectOrderQueueComponent* value =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        domainState().worldState().m_registry, *entity))
                record.orderQueue = *value;
            if (const ObjectSystemPathSequenceComponent* value =
                    ecs::try_get<ObjectSystemPathSequenceComponent>(
                        domainState().worldState().m_registry, *entity))
                record.systemPathSequence = *value;
            if (const ObjectAirborneComponent* value =
                    ecs::try_get<ObjectAirborneComponent>(
                        domainState().worldState().m_registry, *entity))
                record.airborne = *value;
            if (const ObjectPendingPlayerEvacuationComponent* value =
                    ecs::try_get<ObjectPendingPlayerEvacuationComponent>(
                        domainState().worldState().m_registry, *entity))
                record.playerEvacuation = *value;
            if (const PrimaryTeamComponent* value =
                    ecs::try_get<PrimaryTeamComponent>(
                        domainState().worldState().m_registry, *entity))
                record.primaryTeam = *value;
        }
        if (!appendValidatedObjectAIPathSequence(
                record.object, record.orderQueue,
                record.systemPathSequence, pathSequenceValidation,
                pathSequenceScratch)) {
            return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
        }
        candidate.orderOwners.push_back(std::move(record));
    }

    const auto economyView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectEconomyComponent>(domainState().worldState().m_registry);
    candidate.economyObjects.reserve(economyView.size_hint());
    for (const ecs::entity entity : economyView) {
        const ObjectIdentityComponent& identity =
            ecs::get<const ObjectIdentityComponent>(domainState().worldState().m_registry, entity);
        const ObjectEconomyComponent& component =
            ecs::get<const ObjectEconomyComponent>(domainState().worldState().m_registry, entity);
        ObjectAIWorldEconomySnapshot record;
        record.object = identity.id;
        if (!record.object ||
            captureSnapshot(component, record.runtime) !=
                ObjectEconomySnapshotStatus::Success) {
            return ObjectAIWorldSnapshotStatus::EconomyRejected;
        }
        candidate.economyObjects.push_back(std::move(record));
    }
    size_t economyComponentCount = 0;
    for (const ecs::entity entity :
         ecs::view<const ObjectEconomyComponent>(domainState().worldState().m_registry)) {
        static_cast<void>(entity);
        ++economyComponentCount;
    }
    if (economyComponentCount != candidate.economyObjects.size())
        return ObjectAIWorldSnapshotStatus::EconomyRejected;
    std::sort(candidate.economyObjects.begin(),
              candidate.economyObjects.end(),
              [](const ObjectAIWorldEconomySnapshot& left,
                 const ObjectAIWorldEconomySnapshot& right) {
                  return left.object < right.object;
              });
    for (size_t index = 1; index < candidate.economyObjects.size(); ++index) {
        if (!(candidate.economyObjects[index - 1].object <
              candidate.economyObjects[index].object)) {
            return ObjectAIWorldSnapshotStatus::InvalidObjectOrder;
        }
    }

    candidate.strategicBuildEntries.reserve(
        domainState().aiState().m_priorityBuildEntries.size());
    for (const GameSessionPriorityBuildEntry& entry :
         domainState().aiState().m_priorityBuildEntries) {
        candidate.strategicBuildEntries.push_back({
            .player = entry.player,
            .objectType = entry.objectType,
            .anchorX = entry.anchorX,
            .anchorY = entry.anchorY,
            .yawRadians = entry.yawRadians,
            .scriptName = entry.scriptName,
            .sourceSideOrdinal = entry.sourceSideOrdinal,
            .sourceBuildListOrdinal = entry.sourceBuildListOrdinal,
            .sourceSequence = entry.sourceSequence,
            .createdTick = entry.createdTick,
            .nextAttemptTick = entry.nextAttemptTick,
            .attemptCount = entry.attemptCount,
            .placementSearchOrdinal = entry.placementSearchOrdinal,
            .state = static_cast<uint8_t>(entry.state),
            .reservedBuilder = entry.reservedBuilder,
            .constructedObject = entry.constructedObject,
            .remainingRebuilds = entry.remainingRebuilds,
            .strategicPlanId = entry.strategicPlanId,
            .authoredBuildList = entry.authoredBuildList,
        });
    }

    candidate.pendingMoveCompletions = domainState().aiState().m_objectAIMoveCompletions;
    for (size_t index = 0;
         index < candidate.pendingMoveCompletions.size(); ++index) {
        const ai::PathCorrelation& correlation =
            candidate.pendingMoveCompletions[index];
        if (!correlation.isValid() ||
            !correlation.orderIdentity.isValid() ||
            correlation.subject != correlation.orderIdentity.subject) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (candidate.pendingMoveCompletions[earlier] == correlation)
                return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
    }

    output = std::move(candidate);
    return ObjectAIWorldSnapshotStatus::Success;
}

ObjectAIWorldSnapshotStatus GameSessionAIDomain::restoreObjectAIWorldSnapshot(
    const ObjectAIWorldSnapshot& snapshot) {
    if (!domainState().contentState().m_active)
        return ObjectAIWorldSnapshotStatus::SessionInactive;
    if (domainState().contentState().m_drainingGameplayWork)
        return ObjectAIWorldSnapshotStatus::Busy;
    if (snapshot.schemaVersion != ObjectAIWorldSnapshot::SchemaVersion)
        return ObjectAIWorldSnapshotStatus::InvalidSchema;
    if ((!snapshot.hasConfirmedFrame && snapshot.confirmedTick != 0) ||
        (snapshot.hasConfirmedFrame && snapshot.confirmedTick == 0)) {
        return ObjectAIWorldSnapshotStatus::InvalidTickState;
    }
    if (snapshot.aiRuntime.latestInput.valid &&
        snapshot.aiRuntime.latestInput.confirmedTick >
            snapshot.confirmedTick) {
        return ObjectAIWorldSnapshotStatus::InvalidTickState;
    }

    // Restore both stateful service owners into detached candidates first.
    // Their own restore routines validate schema, capacities, free lists and
    // internal indices before publishing a replacement owner.
    ai::ObjectAIRuntime aiCandidate;
    if (aiCandidate.restoreSnapshot(snapshot.aiRuntime) !=
            ai::ObjectAIRuntimeSnapshotStatus::Success) {
        return ObjectAIWorldSnapshotStatus::AIRuntimeRejected;
    }
    StrategicAIRuntime strategicCandidate;
    if (!strategicCandidate.restoreSnapshot(snapshot.strategicAI)) {
        return ObjectAIWorldSnapshotStatus::StrategicAIRejected;
    }
    ObjectTeamRegistry objectTeamsCandidate;
    if (!objectTeamsCandidate.restoreSnapshot(snapshot.objectTeams)) {
        return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
    }
    for (const ai::ObjectAIRecipeBindingSnapshot& binding :
         aiCandidate.recipeBindings()) {
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(binding.subject);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(
                  domainState().worldState().m_registry, *entity)
            : nullptr;
        const ai::AIRecipeId recipe =
            type && type->archetype && type->archetype->hasAiUpdate
            ? type->archetype->aiRecipe
            : ai::AIRecipeId::Invalid;
        const bool matches = binding.state ==
                ai::ObjectAIRecipeBindingState::Bound
            ? recipe != ai::AIRecipeId::Invalid && recipe == binding.recipe
            : binding.state ==
                  ai::ObjectAIRecipeBindingState::ContentUnavailable &&
                  recipe == ai::AIRecipeId::Invalid;
        if (!matches)
            return ObjectAIWorldSnapshotStatus::RecipeMismatch;
    }
    navigation::NavigationSystem navigationCandidate;
    if (navigationCandidate.restoreSnapshot(snapshot.navigation) !=
            navigation::NavigationSystemStatus::Success) {
        return ObjectAIWorldSnapshotStatus::NavigationRejected;
    }

    const container::Vector<ObjectId> restoredOwners =
        objectAIWorldOrderSubjects(aiCandidate, domainState().worldState().m_registry);
    if (restoredOwners.size() != snapshot.orderOwners.size())
        return ObjectAIWorldSnapshotStatus::ObjectSetMismatch;

    struct MovementCommit final {
        ecs::entity entity{};
        ObjectAIWorldOrderOwnerSnapshot value;
        std::optional<ObjectLocomotionComponent> locomotion;
    };
    container::Vector<MovementCommit> movementCommits;
    movementCommits.reserve(snapshot.orderOwners.size());
    container::Vector<ObjectId> primaryTeamChanges;
    primaryTeamChanges.reserve(snapshot.orderOwners.size());
    ObjectId previousOwner = INVALID_OBJECT_ID;
    for (size_t index = 0; index < snapshot.orderOwners.size(); ++index) {
        const ObjectAIWorldOrderOwnerSnapshot& record =
            snapshot.orderOwners[index];
        if (!record.object ||
            (previousOwner && !(previousOwner < record.object))) {
            return ObjectAIWorldSnapshotStatus::InvalidObjectOrder;
        }
        previousOwner = record.object;
        if (restoredOwners[index] != record.object)
            return ObjectAIWorldSnapshotStatus::ObjectSetMismatch;

        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(record.object);
        if (record.objectPresent != entity.has_value())
            return ObjectAIWorldSnapshotStatus::ObjectSetMismatch;
        if (!entity) {
            if (record.fixedTransform || record.locomotion ||
                record.pathMovement ||
                record.movementObstruction ||
                record.temporaryCollisionIgnore ||
                record.waypointCompletion ||
                record.orderQueue || record.systemPathSequence ||
                record.airborne || record.playerEvacuation ||
                record.primaryTeam) {
                return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
            }
            continue;
        }

        const bool presenceMatches =
            record.fixedTransform.has_value() ==
                (ecs::try_get<ObjectFixedTransformComponent>(
                     domainState().worldState().m_registry, *entity) !=
                 nullptr) &&
            record.locomotion.has_value() ==
                (ecs::try_get<ObjectLocomotionComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr) &&
            record.pathMovement.has_value() ==
                (ecs::try_get<ObjectAIPathMovementComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr) &&
            record.orderQueue.has_value() ==
                (ecs::try_get<ObjectOrderQueueComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr) &&
            record.systemPathSequence.has_value() ==
                (ecs::try_get<ObjectSystemPathSequenceComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr) &&
            record.airborne.has_value() ==
                (ecs::try_get<ObjectAirborneComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr) &&
            record.primaryTeam.has_value() ==
                (ecs::try_get<PrimaryTeamComponent>(
                     domainState().worldState().m_registry, *entity) != nullptr);
        if (!presenceMatches)
            return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
        if (record.orderQueue &&
            record.orderQueue->orders.size() >
                ObjectOrderQueueComponent::MaximumQueuedOrders) {
            return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
        }
        if (record.pathMovement &&
            (!record.pathMovement->correlation.isValid() ||
             !record.pathMovement->correlation.orderIdentity.isValid() ||
             record.pathMovement->correlation.subject != record.object ||
             record.pathMovement->correlation.orderIdentity.subject !=
                 record.object ||
             !record.pathMovement->path ||
             record.pathMovement->pathRevision == 0)) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        if (record.movementObstruction &&
            (!record.movementObstruction->blocker ||
             record.movementObstruction->blocker == record.object ||
             record.movementObstruction->lastContactTick == 0 ||
             record.movementObstruction->lastContactTick >
                 snapshot.confirmedTick ||
             record.movementObstruction->consecutiveTicks == 0)) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        if (record.temporaryCollisionIgnore &&
            (record.temporaryCollisionIgnore->untilTick <=
                 snapshot.confirmedTick ||
             record.temporaryCollisionIgnore->other == record.object)) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        if (record.waypointCompletion &&
            (record.waypointCompletion->terminalWaypointId ==
                 std::numeric_limits<uint32_t>::max() ||
             record.waypointCompletion->waypointGraphRevision !=
                 domainState().contentState().m_terrain.
                     waypointGraphRevision() ||
             record.waypointCompletion->completedAtTick >
                 snapshot.confirmedTick ||
             !domainState().contentState().m_terrain.waypointById(
                 record.waypointCompletion->terminalWaypointId))) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        std::optional<ObjectLocomotionComponent> stagedLocomotion;
        if (record.locomotion) {
            const ObjectLocomotionComponent* current =
                ecs::try_get<ObjectLocomotionComponent>(
                    domainState().worldState().m_registry, *entity);
            ObjectLocomotionComponent candidate;
            if (!current || !stageLocomotionRestore(
                    *record.locomotion, *current,
                    domainState().contentState().m_contentSnapshot,
                    candidate)) {
                return ObjectAIWorldSnapshotStatus::
                    ComponentPresenceMismatch;
            }
            stagedLocomotion = std::move(candidate);
        }
        movementCommits.push_back(
            {*entity, record, std::move(stagedLocomotion)});
    }
    for (const MovementCommit& commit : movementCommits) {
        const std::optional<ObjectTeamId> restoredTeam =
            objectTeamsCandidate.teamOf(commit.value.object);
        if (commit.value.primaryTeam
                ? !restoredTeam || *restoredTeam !=
                      commit.value.primaryTeam->team
                : restoredTeam.has_value()) {
            return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
        }
        if (restoredTeam) {
            const ObjectTeamRecord* team =
                objectTeamsCandidate.find(*restoredTeam);
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(
                    domainState().worldState().m_registry,
                    commit.entity);
            // Player ownership belongs to the wider world/script checkpoint,
            // not this AI-owned slice. Refuse to splice a Team snapshot across
            // an ownership change instead of bypassing capture side effects.
            if (!team || !owner || owner->player != team->owner) {
                return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
            }
        }
        const PrimaryTeamComponent* currentTeam =
            ecs::try_get<PrimaryTeamComponent>(
                domainState().worldState().m_registry, commit.entity);
        if (commit.value.primaryTeam && currentTeam &&
            commit.value.primaryTeam->team != currentTeam->team) {
            primaryTeamChanges.push_back(commit.value.object);
        }
    }
    for (const ObjectTeamRecord& team : objectTeamsCandidate.teams()) {
        if (!team.id) continue;
        if (const auto attackPriority =
                objectTeamsCandidate.attackPrioritySet(team.id);
            attackPriority &&
            domainState().presentationState().m_scriptAttackPrioritySets.find(
                container::String{*attackPriority}) ==
                domainState().presentationState()
                    .m_scriptAttackPrioritySets.end()) {
            return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
        }
        if (team.commonTarget &&
            !domainState().worldState().m_objects.entityFromId(
                team.commonTarget)) {
            return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
        }
        for (const ObjectId reinforcement : team.pendingReinforcements) {
            if (!domainState().worldState().m_objects.entityFromId(
                    reinforcement)) {
                return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
            }
        }
        for (const ObjectId member : team.members.values()) {
            if (!std::binary_search(restoredOwners.begin(),
                                    restoredOwners.end(), member) ||
                !domainState().worldState().m_objects.entityFromId(member)) {
                return ObjectAIWorldSnapshotStatus::ObjectTeamsRejected;
            }
        }
    }

    struct EconomyCommit final {
        ecs::entity entity{};
        ObjectEconomyComponent value;
    };
    container::Vector<ObjectId> currentEconomyObjects;
    container::Vector<EconomyCommit> economyCommits;
    const auto economyView = ecs::view<
        const ObjectIdentityComponent,
        ObjectEconomyComponent>(domainState().worldState().m_registry);
    currentEconomyObjects.reserve(economyView.size_hint());
    for (const ecs::entity entity : economyView) {
        const ObjectId object = ecs::get<const ObjectIdentityComponent>(
            domainState().worldState().m_registry, entity).id;
        if (!object)
            return ObjectAIWorldSnapshotStatus::EconomyRejected;
        currentEconomyObjects.push_back(object);
    }
    size_t economyComponentCount = 0;
    for (const ecs::entity entity :
         ecs::view<const ObjectEconomyComponent>(domainState().worldState().m_registry)) {
        static_cast<void>(entity);
        ++economyComponentCount;
    }
    if (economyComponentCount != currentEconomyObjects.size())
        return ObjectAIWorldSnapshotStatus::EconomyRejected;
    std::sort(currentEconomyObjects.begin(), currentEconomyObjects.end());
    if (currentEconomyObjects.size() != snapshot.economyObjects.size())
        return ObjectAIWorldSnapshotStatus::ObjectSetMismatch;

    ObjectId previousEconomy = INVALID_OBJECT_ID;
    economyCommits.reserve(snapshot.economyObjects.size());
    for (size_t index = 0; index < snapshot.economyObjects.size(); ++index) {
        const ObjectAIWorldEconomySnapshot& record =
            snapshot.economyObjects[index];
        if (!record.object ||
            (previousEconomy && !(previousEconomy < record.object))) {
            return ObjectAIWorldSnapshotStatus::InvalidObjectOrder;
        }
        previousEconomy = record.object;
        if (currentEconomyObjects[index] != record.object)
            return ObjectAIWorldSnapshotStatus::ObjectSetMismatch;
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromIdIncludingPending(record.object);
        ObjectEconomyComponent* current = entity
            ? ecs::try_get<ObjectEconomyComponent>(domainState().worldState().m_registry, *entity)
            : nullptr;
        if (!current)
            return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
        ObjectEconomyComponent candidate = *current;
        if (restoreSnapshot(candidate, record.runtime) !=
                ObjectEconomySnapshotStatus::Success) {
            return ObjectAIWorldSnapshotStatus::EconomyRejected;
        }
        economyCommits.push_back({*entity, std::move(candidate)});
    }

    container::Vector<ai::PathCorrelation> completionCandidate =
        snapshot.pendingMoveCompletions;
    for (size_t index = 0; index < completionCandidate.size(); ++index) {
        const ai::PathCorrelation& correlation = completionCandidate[index];
        if (!correlation.isValid() ||
            !correlation.orderIdentity.isValid() ||
            correlation.subject != correlation.orderIdentity.subject ||
            !std::binary_search(restoredOwners.begin(), restoredOwners.end(),
                                correlation.subject)) {
            return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (completionCandidate[earlier] == correlation)
                return ObjectAIWorldSnapshotStatus::InvalidCorrelation;
        }
    }
    if (snapshot.strategicBuildEntries.size() > 4096u)
        return ObjectAIWorldSnapshotStatus::StrategicAIRejected;
    container::Vector<GameSessionPriorityBuildEntry>
        strategicBuildCandidate;
    strategicBuildCandidate.reserve(
        snapshot.strategicBuildEntries.size());
    for (size_t index = 0;
         index < snapshot.strategicBuildEntries.size(); ++index) {
        const ObjectAIStrategicBuildEntrySnapshot& value =
            snapshot.strategicBuildEntries[index];
        const StrategicAIBuildPlan* strategicPlan =
            value.strategicPlanId != 0
            ? strategicCandidate.findBuildPlan(value.strategicPlanId)
            : nullptr;
        if (!value.player || value.objectType.empty() ||
            value.remainingRebuilds < -1 ||
            value.state > static_cast<uint8_t>(
                GameSessionPriorityBuildState::Exhausted) ||
            (value.strategicPlanId != 0 &&
             (!strategicPlan || strategicPlan->player != value.player ||
              strategicPlan->objectType != value.objectType))) {
            return ObjectAIWorldSnapshotStatus::StrategicAIRejected;
        }
        for (size_t earlier = 0; earlier < index; ++earlier) {
            const ObjectAIStrategicBuildEntrySnapshot& previous =
                snapshot.strategicBuildEntries[earlier];
            const bool duplicateStrategic = value.strategicPlanId != 0 &&
                value.strategicPlanId == previous.strategicPlanId;
            const bool duplicateAuthored = value.authoredBuildList &&
                previous.authoredBuildList &&
                value.sourceSideOrdinal == previous.sourceSideOrdinal &&
                value.sourceBuildListOrdinal ==
                    previous.sourceBuildListOrdinal;
            if (duplicateStrategic || duplicateAuthored)
                return ObjectAIWorldSnapshotStatus::StrategicAIRejected;
        }
        strategicBuildCandidate.push_back({
            .player = value.player,
            .objectType = value.objectType,
            .anchorX = value.anchorX,
            .anchorY = value.anchorY,
            .yawRadians = value.yawRadians,
            .scriptName = value.scriptName,
            .sourceSideOrdinal = value.sourceSideOrdinal,
            .sourceBuildListOrdinal = value.sourceBuildListOrdinal,
            .sourceSequence = value.sourceSequence,
            .createdTick = value.createdTick,
            .nextAttemptTick = value.nextAttemptTick,
            .attemptCount = value.attemptCount,
            .placementSearchOrdinal = value.placementSearchOrdinal,
            .state = static_cast<GameSessionPriorityBuildState>(
                value.state),
            .reservedBuilder = value.reservedBuilder,
            .constructedObject = value.constructedObject,
            .remainingRebuilds = value.remainingRebuilds,
            .strategicPlanId = value.strategicPlanId,
            .authoredBuildList = value.authoredBuildList,
        });
    }
    // The flattened FollowPath projection is derived from the snapshotted
    // order queue and cold system-route component. Rebuild it instead of
    // serializing an opaque resolver backing store.
    ai::ObjectAIPathSequenceSnapshot pathSequencesCandidate;
    container::Vector<ai::AIFixedPosition> pathSequenceScratch;
    for (const MovementCommit& commit : movementCommits) {
        const ObjectAIWorldOrderOwnerSnapshot& record = commit.value;
        if (!appendValidatedObjectAIPathSequence(
                record.object, record.orderQueue,
                record.systemPathSequence, pathSequencesCandidate,
                pathSequenceScratch)) {
            return ObjectAIWorldSnapshotStatus::ComponentPresenceMismatch;
        }
    }

    // Commit begins only after the complete section has passed validation and
    // all allocating copies have been staged. These writes are an explicit
    // restore boundary, not a second tick-time gameplay writer.
    domainState().aiState().m_objectAIPathSequences = std::move(pathSequencesCandidate);
    for (MovementCommit& commit : movementCommits) {
        if (commit.value.fixedTransform) {
            writeAuthoritativeObjectTransform(
                domainState().worldState().m_registry, commit.entity,
                commit.value.fixedTransform->position,
                commit.value.fixedTransform->yawRadians);
        }
        if (commit.locomotion)
            *ecs::try_get<ObjectLocomotionComponent>(domainState().worldState().m_registry,
                                                     commit.entity) =
                std::move(*commit.locomotion);
        if (commit.value.pathMovement)
            *ecs::try_get<ObjectAIPathMovementComponent>(domainState().worldState().m_registry,
                                                         commit.entity) =
                std::move(*commit.value.pathMovement);
        if (commit.value.movementObstruction) {
            if (ObjectAIMovementObstructionStateComponent* current =
                    ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                        domainState().worldState().m_registry,
                        commit.entity)) {
                *current = *commit.value.movementObstruction;
            } else {
                ecs::emplace<ObjectAIMovementObstructionStateComponent>(
                    domainState().worldState().m_registry, commit.entity,
                    *commit.value.movementObstruction);
            }
        } else {
            ecs::remove<ObjectAIMovementObstructionStateComponent>(
                domainState().worldState().m_registry, commit.entity);
        }
        if (commit.value.temporaryCollisionIgnore) {
            if (ObjectTemporaryCollisionIgnoreComponent* current =
                    ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                        domainState().worldState().m_registry,
                        commit.entity)) {
                *current = *commit.value.temporaryCollisionIgnore;
            } else {
                ecs::emplace<ObjectTemporaryCollisionIgnoreComponent>(
                    domainState().worldState().m_registry, commit.entity,
                    *commit.value.temporaryCollisionIgnore);
            }
        } else {
            ecs::remove<ObjectTemporaryCollisionIgnoreComponent>(
                domainState().worldState().m_registry, commit.entity);
        }
        if (commit.value.repulsorExpiry) {
            if (ObjectRepulsorExpiryComponent* current =
                    ecs::try_get<ObjectRepulsorExpiryComponent>(
                        domainState().worldState().m_registry,
                        commit.entity)) {
                *current = *commit.value.repulsorExpiry;
            } else {
                ecs::emplace<ObjectRepulsorExpiryComponent>(
                    domainState().worldState().m_registry, commit.entity,
                    *commit.value.repulsorExpiry);
            }
        } else {
            ecs::remove<ObjectRepulsorExpiryComponent>(
                domainState().worldState().m_registry, commit.entity);
        }
        if (commit.value.waypointCompletion) {
            if (ObjectWaypointCompletionComponent* current =
                    ecs::try_get<ObjectWaypointCompletionComponent>(
                        domainState().worldState().m_registry,
                        commit.entity)) {
                *current = *commit.value.waypointCompletion;
            } else {
                ecs::emplace<ObjectWaypointCompletionComponent>(
                    domainState().worldState().m_registry, commit.entity,
                    *commit.value.waypointCompletion);
            }
        } else {
            ecs::remove<ObjectWaypointCompletionComponent>(
                domainState().worldState().m_registry, commit.entity);
        }
        if (commit.value.orderQueue)
            *ecs::try_get<ObjectOrderQueueComponent>(domainState().worldState().m_registry,
                                                     commit.entity) =
                std::move(*commit.value.orderQueue);
        if (commit.value.systemPathSequence)
            *ecs::try_get<ObjectSystemPathSequenceComponent>(
                domainState().worldState().m_registry, commit.entity) =
                std::move(*commit.value.systemPathSequence);
        if (commit.value.airborne)
            *ecs::try_get<ObjectAirborneComponent>(
                domainState().worldState().m_registry, commit.entity) =
                *commit.value.airborne;
        if (commit.value.playerEvacuation) {
            if (ObjectPendingPlayerEvacuationComponent* current =
                    ecs::try_get<ObjectPendingPlayerEvacuationComponent>(
                        domainState().worldState().m_registry,
                        commit.entity)) {
                *current = *commit.value.playerEvacuation;
            } else {
                ecs::emplace<ObjectPendingPlayerEvacuationComponent>(
                    domainState().worldState().m_registry, commit.entity,
                    *commit.value.playerEvacuation);
            }
        } else {
            ecs::remove<ObjectPendingPlayerEvacuationComponent>(
                domainState().worldState().m_registry, commit.entity);
        }
        if (commit.value.primaryTeam)
            *ecs::try_get<PrimaryTeamComponent>(
                domainState().worldState().m_registry, commit.entity) =
                *commit.value.primaryTeam;
    }
    for (EconomyCommit& commit : economyCommits) {
        *ecs::try_get<ObjectEconomyComponent>(domainState().worldState().m_registry, commit.entity) =
            std::move(commit.value);
    }
    domainState().contentState().m_navigation = std::move(navigationCandidate);
    domainState().aiState().m_objectAI = std::move(aiCandidate);
    domainState().aiState().m_strategicAI =
        std::move(strategicCandidate);
    domainState().worldState().m_objectTeams =
        std::move(objectTeamsCandidate);
    for (const ObjectTeamRecord& team :
         domainState().worldState().m_objectTeams.teams()) {
        if (!team.id) continue;
        ObjectAIAttitude inheritedAttitude = ObjectAIAttitude::Normal;
        if (domainState().presentationState().m_scenarioDefinition) {
            for (const scenario::ScriptTeamDefinition& definition :
                 domainState().presentationState().m_scenarioDefinition->scriptTeams()) {
                const container::Span<const ObjectTeamId> instances =
                    domainState().worldState().m_objectTeams.
                        scenarioTeamInstances(definition.id);
                if (std::find(instances.begin(), instances.end(), team.id) ==
                    instances.end()) {
                    continue;
                }
                inheritedAttitude = static_cast<ObjectAIAttitude>(
                    std::clamp(definition.plan.initialAttitude, -2, 2));
                break;
            }
        }
        uint32_t attackPrioritySetId = 0;
        if (const auto attackPriority =
                domainState().worldState().m_objectTeams.attackPrioritySet(
                    team.id)) {
            const auto found =
                domainState().presentationState().m_scriptAttackPrioritySets.find(
                    container::String{*attackPriority});
            if (found != domainState().presentationState()
                             .m_scriptAttackPrioritySets.end()) {
                attackPrioritySetId = found->second.id;
            }
        }
        for (const ObjectId member : team.members.values()) {
            const std::optional<ecs::entity> entity =
                domainState().worldState().m_objects.entityFromId(member);
            if (!entity) continue;
            if (team.relationshipPolicy) {
                if (ObjectRelationshipOverrideComponent* current =
                        ecs::try_get<ObjectRelationshipOverrideComponent>(
                            domainState().worldState().m_registry, *entity)) {
                    current->policy = team.relationshipPolicy;
                } else {
                    ecs::emplace<ObjectRelationshipOverrideComponent>(
                        domainState().worldState().m_registry, *entity,
                        ObjectRelationshipOverrideComponent{
                            .policy = team.relationshipPolicy,
                        });
                }
            } else {
                ecs::remove<ObjectRelationshipOverrideComponent>(
                    domainState().worldState().m_registry, *entity);
            }
            if (!std::binary_search(primaryTeamChanges.begin(),
                                    primaryTeamChanges.end(), member)) {
                continue;
            }
            if (ObjectAIBehaviorPolicyComponent* behaviorPolicy =
                    ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                        domainState().worldState().m_registry, *entity)) {
                if (behaviorPolicy->attackPrioritySetId !=
                        attackPrioritySetId ||
                    behaviorPolicy->attitude != inheritedAttitude) {
                    behaviorPolicy->attackPrioritySetId =
                        attackPrioritySetId;
                    behaviorPolicy->attitude = inheritedAttitude;
                    ++behaviorPolicy->revision;
                    if (behaviorPolicy->revision == 0)
                        ++behaviorPolicy->revision;
                }
            } else if (attackPrioritySetId != 0 ||
                       inheritedAttitude != ObjectAIAttitude::Normal) {
                ecs::emplace<ObjectAIBehaviorPolicyComponent>(
                    domainState().worldState().m_registry, *entity,
                    ObjectAIBehaviorPolicyComponent{
                        .attackPrioritySetId = attackPrioritySetId,
                        .attitude = inheritedAttitude,
                    });
            }
        }
    }
    domainState().aiState().m_objectAI.setPathSequenceResolver(domainState().aiState().m_objectAIPathSequences.resolver());
    const ai::AIWaypointGraphResolver waypointResolver =
        terrainWaypointResolver(domainState().contentState().m_terrain);
    domainState().aiState().m_objectAI.setWaypointGraphResolver(
        waypointResolver);
    domainState().contentState().m_navigation.setWaypointGraphResolver(
        waypointResolver);
    domainState().aiState().m_objectAI.setPathHandleReleaser({
        .context = &domainState().contentState().m_navigation,
        .release = [](void* context, ai::PathHandle path) noexcept {
            auto* navigation =
                static_cast<navigation::NavigationSystem*>(context);
            if (navigation && navigation->isInitialized()) {
                static_cast<void>(navigation->releasePath(
                    path, navigation->pathRevision()));
            }
        },
    });
    domainState().aiState().m_objectAI.setPathfindCellSizeRaw(
        domainState().contentState().m_navigation.grid().transform().cellSizeRaw);
    domainState().aiState().m_objectAIMoveCompletions = std::move(completionCandidate);
    domainState().aiState().m_priorityBuildEntries =
        std::move(strategicBuildCandidate);
    domainState().aiState().m_objectAIShadowFacts.clear();
    domainState().aiState().m_objectAIShadowNextFacts.clear();
    domainState().aiState().m_objectAIMovementCommands.clear();
    domainState().presentationState().m_confirmedTick = snapshot.confirmedTick;
    domainState().presentationState().m_hasConfirmedFrame = snapshot.hasConfirmedFrame;
    return ObjectAIWorldSnapshotStatus::Success;
}

ObjectAISimulationDigest GameSessionAIDomain::objectAISimulationDigest() const {
    ObjectAISimulationDigest result;
    if (!domainState().contentState().m_active || !domainState().aiState().m_objectAI.initialized() ||
        !domainState().contentState().m_navigation.isInitialized())
        return result;

    ai::ObjectAIRuntimeSnapshot snapshot;
    if (domainState().aiState().m_objectAI.captureSnapshot(snapshot) ==
        ai::ObjectAIRuntimeSnapshotStatus::Success)
        result.aiRuntime = ai::stableDigest(snapshot);
    result.navigation = domainState().contentState().m_navigation.stableHash();

    uint64_t movement = 14695981039346656037ull;
    const auto byte = [&movement](uint8_t value) noexcept {
        movement ^= value;
        movement *= 1099511628211ull;
    };
    const auto u32 = [&byte](uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    const auto u64 = [&byte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>((value >> shift) & 0xffull));
    };
    const auto string = [&u64, &byte](container::StringView value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (const char character : value)
            byte(static_cast<uint8_t>(character));
    };
    const auto encodeOrderIdentity = [&u32, &u64](
        const ai::AIAsyncOrderIdentity& value) noexcept {
        u32(value.subject.value);
        u64(value.queueRevision);
        u64(value.externalRevision);
        u64(value.issuedTick);
        u32(value.sourceSequence);
        u32(value.sourceScriptId);
        u32(value.systemPurposeInstance);
        u32(value.source);
        u32(value.systemPurpose);
    };
    const auto encodePathCorrelation =
        [&u32, &u64, &encodeOrderIdentity](
            const ai::PathCorrelation& value) noexcept {
            u32(value.subject.value);
            u64(value.stateRequest.issuedTick);
            u32(value.stateRequest.sequence);
            u32(value.generation);
            u64(value.sourceOrderRevision);
            encodeOrderIdentity(value.orderIdentity);
        };

    // v18 includes the one-script-pass waypoint completion pulse. It changes
    // authored conditions on the following tick and is part of rollback/CRC.
    // v13 replaces the TransformComponent float projection with the
    // authoritative fixed transform in the movement digest.
    // v12 adds the complete ObjectTeamRegistry hash and PrimaryTeam
    // projection; team activation, ownership, membership, script state,
    // pulses and bindings can no longer alias in the AI world digest.
    // v16 adds frozen AIUpdate idle-acquire policy and selected-target facts.
    // v15 hashes every simulation-authoritative Locomotor scalar from the
    // load-time Q32.32 record. Remaining float fields are presentation-only
    // profile identity and never feed movement/controller decisions.
    // v14 stops hashing ObjectLocomotionComponent float compatibility mirrors
    // as runtime authority; active mutable state uses Q32.32 columns.
    // v11 adds the persistent strategic AI and Scenario Team WorkOrder
    // producer/backoff state.
    // v10 adds path-through-unit policy plus the deterministic moving-pair
    // obstruction ledger and pair-scoped temporary collision overlay.
    // v9 adds the pending player evacuation continuation, airborne fact, and
    // precise-Z locomotor switch. They determine whether passengers may leave
    // an aircraft and therefore belong to the stable simulation digest.
    // v8 encodes the complete frozen LocomotorSet plus air/Z controller
    // state. A future surface transition can select a profile that is not
    // currently active, so hashing only templateName is insufficient.
    // v7 also encodes script-owned AI mood/attack-priority policy and the
    // mutable AttackPrioritySet catalog. These values alter future target
    // acquisition even when the current order and transform are unchanged.
    // v6 added the complete locomotor policy. Runtime upgrades can
    // change these values and thereby alter future movement even while the
    // current position/order queue remains identical.
    // v19 adds player-created formation identity/offsets. They affect the
    // next positional player order even while transform/order queue is
    // unchanged, so omitting them would hide a future lockstep divergence.
    u32(19);
    u64(domainState().presentationState().m_confirmedTick);
    byte(domainState().presentationState().m_hasConfirmedFrame ? uint8_t{1} : uint8_t{0});
    const container::Vector<ObjectId> owners =
        objectAIWorldOrderSubjects(domainState().aiState().m_objectAI, domainState().worldState().m_registry);
    u64(static_cast<uint64_t>(owners.size()));
    for (const ObjectId subject : owners) {
        u32(subject.value);
        const std::optional<ecs::entity> entity =
            domainState().worldState().m_objects.entityFromId(subject);
        byte(entity ? uint8_t{1} : uint8_t{0});
        if (!entity) continue;

        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(
                domainState().worldState().m_registry, *entity);
        byte(fixedTransform ? uint8_t{1} : uint8_t{0});
        if (fixedTransform) {
            u64(static_cast<uint64_t>(fixedTransform->position.x.raw()));
            u64(static_cast<uint64_t>(fixedTransform->position.y.raw()));
            u64(static_cast<uint64_t>(fixedTransform->position.z.raw()));
            u64(static_cast<uint64_t>(fixedTransform->yawRadians.raw()));
            byte(fixedTransform->authoritative ? uint8_t{1} : uint8_t{0});
        }
        const ObjectPlayerFormationComponent* formation =
            ecs::try_get<ObjectPlayerFormationComponent>(
                domainState().worldState().m_registry, *entity);
        byte(formation ? uint8_t{1} : uint8_t{0});
        if (formation) {
            u64(formation->id);
            u64(static_cast<uint64_t>(formation->offsetX.raw()));
            u64(static_cast<uint64_t>(formation->offsetY.raw()));
        }
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(domainState().worldState().m_registry, *entity);
        byte(locomotion ? uint8_t{1} : uint8_t{0});
        if (locomotion) {
            u64(static_cast<uint64_t>(locomotion->profiles.size()));
            for (const game::FrozenLocomotorTemplate& profile :
                 locomotion->profiles) {
                string(profile.name);
                u32(profile.surfaces);
                u32(static_cast<uint32_t>(profile.appearance));
                u32(static_cast<uint32_t>(profile.zAxisBehavior));
                u32(static_cast<uint32_t>(profile.groupPriority));
                u64(static_cast<uint64_t>(profile.fixed.maximumSpeed.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.damagedMaximumSpeed.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.maximumTurnRate.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.damagedMaximumTurnRate.raw()));
                u64(static_cast<uint64_t>(profile.fixed.acceleration.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.damagedAcceleration.raw()));
                u64(static_cast<uint64_t>(profile.fixed.lift.raw()));
                u64(static_cast<uint64_t>(profile.fixed.damagedLift.raw()));
                u64(static_cast<uint64_t>(profile.fixed.braking.raw()));
                u64(static_cast<uint64_t>(profile.fixed.minimumSpeed.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.minimumTurnSpeed.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.preferredHeight.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.preferredHeightDamping.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.circlingRadius.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.extra2DFrictionPerSecond.raw()));
                u64(static_cast<uint64_t>(profile.fixed.speedLimitZ.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.maximumThrustAngleRadians.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.accelerationPitchLimitRadians.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.decelerationPitchLimitRadians.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.bounceAngularVelocityRadiansPerSecond.raw()));
                u64(static_cast<uint64_t>(profile.fixed.pitchStiffness.raw()));
                u64(static_cast<uint64_t>(profile.fixed.rollStiffness.raw()));
                u64(static_cast<uint64_t>(profile.fixed.pitchDamping.raw()));
                u64(static_cast<uint64_t>(profile.fixed.rollDamping.raw()));
                u64(static_cast<uint64_t>(profile.fixed.thrustRoll.raw()));
                u64(static_cast<uint64_t>(profile.fixed.thrustWobbleRate.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.thrustMinimumWobble.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.thrustMaximumWobble.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.pitchByZVelocityFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.forwardVelocityPitchFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.lateralVelocityRollFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.forwardAccelerationPitchFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.lateralAccelerationRollFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.uniformAxialDamping.raw()));
                u64(static_cast<uint64_t>(profile.fixed.turnPivotOffset.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.maximumWheelExtension.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.maximumWheelCompression.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.frontWheelTurnAngleRadians.raw()));
                u64(static_cast<uint64_t>(profile.fixed.closeEnough.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.slideIntoPlaceMilliseconds.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.wanderWidthFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.wanderLengthFactor.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.wanderAboutPointRadius.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.rudderCorrectionDegree.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.rudderCorrectionRate.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.elevatorCorrectionDegree.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.elevatorCorrectionRate.raw()));
                u64(static_cast<uint64_t>(
                    profile.fixed.airborneTargetingHeight.raw()));
                byte(profile.closeEnoughDistance3D ? uint8_t{1} : uint8_t{0});
                byte(profile.stickToGround ? uint8_t{1} : uint8_t{0});
                byte(profile.canMoveBackwards ? uint8_t{1} : uint8_t{0});
                byte(profile.locomotorWorksWhenDead ? uint8_t{1} : uint8_t{0});
                byte(profile.allowMotiveForceWhileAirborne ? uint8_t{1} : uint8_t{0});
                byte(profile.apply2DFrictionWhenAirborne ? uint8_t{1} : uint8_t{0});
                byte(profile.downhillOnly ? uint8_t{1} : uint8_t{0});
                byte(profile.hasSuspension ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.accelerationIsInfinite
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.damagedAccelerationIsInfinite
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.brakingIsInfinite
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.hasFiniteBraking
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.hasFiniteSpeedLimitZ
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.hasFiniteAirborneTargetingHeight
                         ? uint8_t{1} : uint8_t{0});
                byte(profile.fixed.preferredHeightIsLowest
                         ? uint8_t{1} : uint8_t{0});
            }
            string(locomotion->templateName);
            u64(static_cast<uint64_t>(locomotion->maximumSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->damagedMaximumSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->maximumTurnRate.raw()));
            u64(static_cast<uint64_t>(locomotion->damagedMaximumTurnRate.raw()));
            u64(static_cast<uint64_t>(locomotion->acceleration.raw()));
            u64(static_cast<uint64_t>(locomotion->damagedAcceleration.raw()));
            u64(static_cast<uint64_t>(locomotion->lift.raw()));
            u64(static_cast<uint64_t>(locomotion->damagedLift.raw()));
            u64(static_cast<uint64_t>(locomotion->braking.raw()));
            u64(static_cast<uint64_t>(locomotion->minimumSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->minimumTurnSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->preferredHeightFixed.raw()));
            u64(static_cast<uint64_t>(
                locomotion->preferredHeightDampingFixed.raw()));
            u64(static_cast<uint64_t>(locomotion->speedLimitZ.raw()));
            u64(static_cast<uint64_t>(locomotion->closeEnough.raw()));
            u64(static_cast<uint64_t>(locomotion->slideIntoPlace.raw()));
            byte(locomotion->accelerationIsInfinite ? uint8_t{1} : uint8_t{0});
            byte(locomotion->damagedAccelerationIsInfinite ? uint8_t{1} : uint8_t{0});
            byte(locomotion->brakingIsInfinite ? uint8_t{1} : uint8_t{0});
            byte(locomotion->hasFiniteBraking ? uint8_t{1} : uint8_t{0});
            byte(locomotion->hasFiniteSpeedLimitZ ? uint8_t{1} : uint8_t{0});
            byte(locomotion->preferredHeightIsLowest ? uint8_t{1} : uint8_t{0});
            byte(locomotion->overWater ? uint8_t{1} : uint8_t{0});
            byte(locomotion->usePreciseZPosition ? uint8_t{1} : uint8_t{0});
            byte(locomotion->ultraAccurate ? uint8_t{1} : uint8_t{0});
            byte(locomotion->fixedRuntimeInitialized ? uint8_t{1} : uint8_t{0});
            u64(static_cast<uint64_t>(locomotion->forwardSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->verticalSpeed.raw()));
            u64(static_cast<uint64_t>(locomotion->groundOffsetFixed.raw()));
            u64(static_cast<uint64_t>(locomotion->goal.x.raw()));
            u64(static_cast<uint64_t>(locomotion->goal.y.raw()));
            u64(static_cast<uint64_t>(locomotion->goal.z.raw()));
            u64(locomotion->activeOrderTick);
            u32(locomotion->activeOrderSequence);
            u32(locomotion->activeSourceScriptId);
            byte(locomotion->hasActiveMove ? uint8_t{1} : uint8_t{0});
            byte(locomotion->movingBackward ? uint8_t{1} : uint8_t{0});
            u32(static_cast<uint32_t>(locomotion->state));
        }
        const ObjectAIPathMovementComponent* path =
            ecs::try_get<ObjectAIPathMovementComponent>(domainState().worldState().m_registry, *entity);
        byte(path ? uint8_t{1} : uint8_t{0});
        if (path) {
            encodePathCorrelation(path->correlation);
            u64(path->path.value);
            u32(path->ignoredObstacle.value);
            u64(path->pathRevision);
            u32(path->nextPointIndex);
            u32(path->blockedTicks);
            u64(static_cast<uint64_t>(path->alongPathDistanceRaw));
            u64(static_cast<uint64_t>(path->speedLimitRaw));
            u64(static_cast<uint64_t>(path->extraDistanceRaw));
            u32(static_cast<uint32_t>(path->mode));
            byte(path->panicking ? uint8_t{1} : uint8_t{0});
            byte(path->allowPathThroughUnits ? uint8_t{1} : uint8_t{0});
        }
        const ObjectAIMovementObstructionStateComponent* obstruction =
            ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                domainState().worldState().m_registry, *entity);
        byte(obstruction ? uint8_t{1} : uint8_t{0});
        if (obstruction) {
            u32(obstruction->blocker.value);
            u32(obstruction->previousBlocker.value);
            u64(obstruction->lastContactTick);
            u32(obstruction->consecutiveTicks);
        }
        const ObjectTemporaryCollisionIgnoreComponent* collisionIgnore =
            ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                domainState().worldState().m_registry, *entity);
        byte(collisionIgnore ? uint8_t{1} : uint8_t{0});
        if (collisionIgnore) {
            u64(collisionIgnore->untilTick);
            u32(collisionIgnore->other.value);
        }
        const ObjectRepulsorExpiryComponent* repulsorExpiry =
            ecs::try_get<ObjectRepulsorExpiryComponent>(
                domainState().worldState().m_registry, *entity);
        byte(repulsorExpiry ? uint8_t{1} : uint8_t{0});
        if (repulsorExpiry)
            u64(repulsorExpiry->clearAtTick);
        const ObjectWaypointCompletionComponent* waypointCompletion =
            ecs::try_get<ObjectWaypointCompletionComponent>(
                domainState().worldState().m_registry, *entity);
        byte(waypointCompletion ? uint8_t{1} : uint8_t{0});
        if (waypointCompletion) {
            u32(waypointCompletion->terminalWaypointId);
            u64(waypointCompletion->waypointGraphRevision);
            u64(waypointCompletion->completedAtTick);
        }
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(domainState().worldState().m_registry, *entity);
        byte(queue ? uint8_t{1} : uint8_t{0});
        if (queue) {
            u64(queue->revision);
            u64(queue->externalRevision);
            u64(queue->replacementExternalRevision);
            u32(static_cast<uint32_t>(queue->replacementExternalSource));
            u32(static_cast<uint32_t>(queue->replacementExternalKind));
            u64(static_cast<uint64_t>(queue->orders.size()));
            for (const ObjectOrderIntent& order : queue->orders) {
                u32(static_cast<uint32_t>(order.kind));
                u32(static_cast<uint32_t>(order.tacticalAttackSubtype));
                u32(static_cast<uint32_t>(order.source));
                byte(order.contextPlayer.value);
                u64(order.issuedTick);
                u32(order.sourceSequence);
                u32(order.sourceScriptId);
                u32(order.targetObject.value);
                u64(static_cast<uint64_t>(order.targetX.raw()));
                u64(static_cast<uint64_t>(order.targetY.raw()));
                u64(static_cast<uint64_t>(order.targetZ.raw()));
                byte(order.hasTargetPosition ? uint8_t{1} : uint8_t{0});
                u64(static_cast<uint64_t>(
                    order.placementYawRadians.raw()));
                u64(static_cast<uint64_t>(order.placementEndX.raw()));
                u64(static_cast<uint64_t>(order.placementEndY.raw()));
                u64(static_cast<uint64_t>(order.placementEndZ.raw()));
                byte(order.hasPlacementEndPosition ? uint8_t{1} : uint8_t{0});
                string(order.contentName);
                byte(order.maximumShots ? uint8_t{1} : uint8_t{0});
                if (order.maximumShots) u32(*order.maximumShots);
                u32(order.shotsFired);
                byte(order.forceAttack ? uint8_t{1} : uint8_t{0});
                byte(order.attackMove ? uint8_t{1} : uint8_t{0});
                byte(order.combatDrop ? uint8_t{1} : uint8_t{0});
                u32(static_cast<uint32_t>(order.moveRouteSubtype));
                u32(order.waypointStartId);
                u64(order.waypointGraphRevision);
                u32(order.waypointTeam.value);
                u64(static_cast<uint64_t>(
                    order.waypointGroupOffsetX.raw()));
                u64(static_cast<uint64_t>(
                    order.waypointGroupOffsetY.raw()));
                u64(static_cast<uint64_t>(
                    order.waypointGroupSpeed.raw()));
                u64(order.groupPathId);
                u32(order.groupPathMemberOrdinal);
                u32(order.groupPathMemberCount);
                u64(static_cast<uint64_t>(order.groupPathStartX.raw()));
                u64(static_cast<uint64_t>(order.groupPathStartY.raw()));
                u64(static_cast<uint64_t>(order.groupPathStartZ.raw()));
                u64(static_cast<uint64_t>(order.groupPathOffsetX.raw()));
                u64(static_cast<uint64_t>(order.groupPathOffsetY.raw()));
                byte(order.allArmyHunt ? uint8_t{1} : uint8_t{0});
                byte(order.useTeamCommonTarget ? uint8_t{1} : uint8_t{0});
                byte(order.guardWithoutPursuit ? uint8_t{1} : uint8_t{0});
                byte(order.guardFlyingOnly ? uint8_t{1} : uint8_t{0});
                u32(order.tacticalTargetTeam.value);
                u32(order.tacticalTargetAreaId);
                u64(order.tacticalTargetRevision);
                u32(static_cast<uint32_t>(order.systemPurpose));
                u32(order.systemPurposeInstance);
            }
        }
        const ObjectScriptContainmentEnterComponent* containmentEnter =
            ecs::try_get<ObjectScriptContainmentEnterComponent>(
                domainState().worldState().m_registry, *entity);
        byte(containmentEnter ? uint8_t{1} : uint8_t{0});
        if (containmentEnter) {
            u32(containmentEnter->target.value);
            u64(containmentEnter->issuedTick);
            u32(containmentEnter->sourceSequence);
            u32(containmentEnter->reservedCapacity);
            byte(containmentEnter->approachAttempts);
            u64(containmentEnter->revision);
        }
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(
                domainState().worldState().m_registry, *entity);
        byte(airborne ? uint8_t{1} : uint8_t{0});
        if (airborne) {
            byte(airborne->isAirborne ? uint8_t{1} : uint8_t{0});
        }
        const ObjectPendingPlayerEvacuationComponent* playerEvacuation =
            ecs::try_get<ObjectPendingPlayerEvacuationComponent>(
                domainState().worldState().m_registry, *entity);
        byte(playerEvacuation ? uint8_t{1} : uint8_t{0});
        if (playerEvacuation) {
            byte(playerEvacuation->player.value);
            u64(playerEvacuation->externalOrderRevision);
            u64(playerEvacuation->issuedTick);
            u64(playerEvacuation->deadlineTick);
            u32(playerEvacuation->sourceSequence);
            u64(static_cast<uint64_t>(playerEvacuation->landingZ.raw()));
            byte(playerEvacuation->previousUsePreciseZPosition
                     ? uint8_t{1} : uint8_t{0});
        }
        const PrimaryTeamComponent* primaryTeam =
            ecs::try_get<PrimaryTeamComponent>(
                domainState().worldState().m_registry, *entity);
        byte(primaryTeam ? uint8_t{1} : uint8_t{0});
        if (primaryTeam) u32(primaryTeam->team.value);
        const ObjectContainmentRuntimeComponent* containmentRuntime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                domainState().worldState().m_registry, *entity);
        byte(containmentRuntime ? uint8_t{1} : uint8_t{0});
        if (containmentRuntime) {
            u32(static_cast<uint32_t>(
                containmentRuntime->evacuationDisposition));
            u64(containmentRuntime->evacuationDispositionRevision);
        }
        const ObjectSystemPathSequenceComponent* systemPath =
            ecs::try_get<ObjectSystemPathSequenceComponent>(
                domainState().worldState().m_registry, *entity);
        byte(systemPath ? uint8_t{1} : uint8_t{0});
        if (systemPath) {
            u32(static_cast<uint32_t>(systemPath->routeSubtype));
            u32(static_cast<uint32_t>(systemPath->systemPurpose));
            u64(systemPath->activeQueueRevision);
            u64(systemPath->activeExternalRevision);
            u32(systemPath->ignoredObstacle.value);
            u64(systemPath->issuedTick);
            u32(systemPath->firstSourceSequence);
            u32(systemPath->queuedOrderCount);
            u64(static_cast<uint64_t>(systemPath->points.size()));
            for (const LogicFixedVec3& point : systemPath->points) {
                u64(static_cast<uint64_t>(point.x.raw()));
                u64(static_cast<uint64_t>(point.y.raw()));
                u64(static_cast<uint64_t>(point.z.raw()));
            }
        }
        const ObjectAIBehaviorPolicyComponent* behaviorPolicy =
            ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                domainState().worldState().m_registry, *entity);
        byte(behaviorPolicy ? uint8_t{1} : uint8_t{0});
        if (behaviorPolicy) {
            u32(behaviorPolicy->attackPrioritySetId);
            u32(static_cast<uint32_t>(
                static_cast<int32_t>(behaviorPolicy->attitude)));
            u64(behaviorPolicy->revision);
        }
        const ObjectAITargetScanWakeComponent* targetScanWake =
            ecs::try_get<ObjectAITargetScanWakeComponent>(
                domainState().worldState().m_registry, *entity);
        byte(targetScanWake ? uint8_t{1} : uint8_t{0});
        if (targetScanWake) {
            u64(targetScanWake->requestedTick);
            u64(targetScanWake->revision);
        }
        const ObjectDifficultyBonusComponent* difficultyBonus =
            ecs::try_get<ObjectDifficultyBonusComponent>(
                domainState().worldState().m_registry, *entity);
        byte(difficultyBonus ? uint8_t{1} : uint8_t{0});
        if (difficultyBonus) {
            byte(difficultyBonus->receiving ? uint8_t{1} : uint8_t{0});
            u64(static_cast<uint64_t>(
                difficultyBonus->appliedHealthMultiplier.raw()));
            byte(static_cast<uint8_t>(
                difficultyBonus->appliedWeaponCondition));
            u64(difficultyBonus->revision);
        }
    }
    u64(static_cast<uint64_t>(domainState().presentationState().m_scriptAttackPrioritySets.size()));
    for (const auto& [name, set] : domainState().presentationState().m_scriptAttackPrioritySets) {
        string(name);
        u32(static_cast<uint32_t>(set.defaultPriority));
        u64(set.revision);
        u64(static_cast<uint64_t>(set.rules.size()));
        for (const GameSessionScriptPresentationState::ScriptAttackPriorityRule&
                 rule : set.rules) {
            u32(static_cast<uint32_t>(rule.mutation));
            string(rule.selector);
            u32(static_cast<uint32_t>(rule.priority));
            u64(rule.sequence);
        }
    }
    u64(domainState().presentationState().m_scriptAttackPrioritySequence);
    byte(domainState().presentationState().m_objectsReceiveDifficultyBonuses ? uint8_t{1} : uint8_t{0});
    byte(domainState().presentationState().m_chooseVictimAlwaysNormal ? uint8_t{1} : uint8_t{0});
    const container::Span<const ObjectTeamRecord> teams =
        domainState().worldState().m_objectTeams.teams();
    u64(static_cast<uint64_t>(teams.size()));
    for (const ObjectTeamRecord& team : teams) {
        u32(team.id.value);
        u64(team.policyRevision);
        u32(static_cast<uint32_t>(team.productionPriority));
        u32(team.commonTarget.value);
        u64(static_cast<uint64_t>(team.pendingReinforcements.size()));
        for (const ObjectId reinforcement : team.pendingReinforcements)
            u32(reinforcement.value);
        byte(static_cast<uint8_t>(team.assemblyKind));
        u64(team.assemblyDeadlineTick);
        u64(team.assemblyStartedTick);
        byte(team.assemblyStartTickKnown ? uint8_t{1} : uint8_t{0});
        u32(team.assemblySourceSequence);
        byte(team.hasProductionStartPulse ? uint8_t{1} : uint8_t{0});
        byte(team.pendingProductionStartPulse ? uint8_t{1} : uint8_t{0});
        u64(team.productionStartedAtConfirmedTick);
        u32(team.productionActionPulseCount);
        u32(team.pendingProductionActionPulseCount);
        u32(team.productionActionWithoutTeamPulseCount);
        u32(team.pendingProductionActionWithoutTeamPulseCount);
        u64(static_cast<uint64_t>(
            team.productionCompletedByUnit.size()));
        for (const uint32_t completed :
             team.productionCompletedByUnit) {
            u32(completed);
        }
        u64(static_cast<uint64_t>(team.productionWorkOrders.size()));
        for (const ObjectTeamProductionWorkOrder& order :
             team.productionWorkOrders) {
            u32(order.producer.value);
            u64(order.nextAttemptTick);
            u32(order.failureCount);
        }
        byte(team.recruitableOverride.has_value() ? uint8_t{1} : uint8_t{0});
        if (team.recruitableOverride)
            byte(*team.recruitableOverride ? uint8_t{1} : uint8_t{0});
        const auto teamAttackPriority =
            domainState().worldState().m_objectTeams.attackPrioritySet(team.id);
        string(teamAttackPriority ? *teamAttackPriority
                                  : container::StringView{});
        byte(team.relationshipPolicy ? uint8_t{1} : uint8_t{0});
        if (team.relationshipPolicy) {
            u64(team.relationshipPolicy->revision);
            u64(static_cast<uint64_t>(
                team.relationshipPolicy->teams.size()));
            for (const ObjectTeamRelationshipOverride& override :
                 team.relationshipPolicy->teams) {
                u32(override.target.value);
                byte(static_cast<uint8_t>(override.relationship));
            }
            u64(static_cast<uint64_t>(
                team.relationshipPolicy->players.size()));
            for (const ObjectPlayerRelationshipOverride& override :
                 team.relationshipPolicy->players) {
                byte(override.target.value);
                byte(static_cast<uint8_t>(override.relationship));
            }
        }
    }
    u64(domainState().worldState().m_objectTeams.stableHash());
    u64(static_cast<uint64_t>(domainState().aiState().m_objectAIMoveCompletions.size()));
    for (const ai::PathCorrelation& correlation :
         domainState().aiState().m_objectAIMoveCompletions) {
        encodePathCorrelation(correlation);
    }
    u64(static_cast<uint64_t>(
        domainState().aiState().m_priorityBuildEntries.size()));
    for (const GameSessionPriorityBuildEntry& entry :
         domainState().aiState().m_priorityBuildEntries) {
        byte(entry.player.value);
        string(entry.objectType);
        u64(static_cast<uint64_t>(entry.anchorX.raw()));
        u64(static_cast<uint64_t>(entry.anchorY.raw()));
        u64(static_cast<uint64_t>(entry.yawRadians.raw()));
        string(entry.scriptName);
        u32(entry.sourceSideOrdinal);
        u32(entry.sourceBuildListOrdinal);
        u32(entry.sourceSequence);
        u64(entry.createdTick);
        u64(entry.nextAttemptTick);
        u32(entry.attemptCount);
        u32(entry.placementSearchOrdinal);
        byte(static_cast<uint8_t>(entry.state));
        u32(entry.reservedBuilder.value);
        u32(entry.constructedObject.value);
        u32(static_cast<uint32_t>(entry.remainingRebuilds));
        u64(entry.strategicPlanId);
        byte(entry.authoredBuildList ? uint8_t{1} : uint8_t{0});
    }
    u64(domainState().aiState().m_strategicAI.stableHash());
    result.movement = movement;

    struct EconomyDigestRecord final {
        ObjectId object = INVALID_OBJECT_ID;
        uint64_t digest = 0;
    };
    container::Vector<EconomyDigestRecord> economyRecords;
    uint64_t economyFailures = 0;
    const auto economyView = ecs::view<
        const ObjectIdentityComponent,
        const ObjectEconomyComponent>(domainState().worldState().m_registry);
    economyRecords.reserve(economyView.size_hint());
    for (const ecs::entity entity : economyView) {
        const ObjectIdentityComponent& identity =
            ecs::get<const ObjectIdentityComponent>(domainState().worldState().m_registry, entity);
        const ObjectEconomyComponent& economy =
            ecs::get<const ObjectEconomyComponent>(domainState().worldState().m_registry, entity);
        ObjectEconomyRuntimeSnapshot economySnapshot;
        if (!identity.id || captureSnapshot(economy, economySnapshot) !=
                ObjectEconomySnapshotStatus::Success) {
            ++economyFailures;
            continue;
        }
        economyRecords.push_back({
            .object = identity.id,
            .digest = stableDigest(economySnapshot),
        });
    }
    std::sort(economyRecords.begin(), economyRecords.end(),
              [](const EconomyDigestRecord& left,
                 const EconomyDigestRecord& right) {
                  return left.object < right.object;
              });
    uint64_t economyDigest = 14695981039346656037ull;
    const auto economyByte = [&economyDigest](uint8_t value) noexcept {
        economyDigest ^= value;
        economyDigest *= 1099511628211ull;
    };
    const auto economyU64 = [&economyByte](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            economyByte(static_cast<uint8_t>((value >> shift) & 0xffull));
    };
    economyU64(2);
    economyU64(economyFailures);
    economyU64(static_cast<uint64_t>(economyRecords.size()));
    for (const EconomyDigestRecord& record : economyRecords) {
        economyU64(record.object.value);
        economyU64(record.digest);
    }
    result.economy = economyDigest;
    result.players = domainState().contentState().m_players.simulationDigest();

    uint64_t combined = 14695981039346656037ull;
    const auto combine = [&combined](uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            combined ^= static_cast<uint8_t>((value >> shift) & 0xffull);
            combined *= 1099511628211ull;
        }
    };
    combine(4);
    combine(result.aiRuntime);
    combine(result.navigation);
    combine(result.movement);
    combine(result.economy);
    combine(result.players);
    result.combined = combined;
    return result;
}

} // namespace engine

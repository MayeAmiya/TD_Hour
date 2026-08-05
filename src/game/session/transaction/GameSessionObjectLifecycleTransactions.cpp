#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/frame/GameSessionNavigationPresentationRules.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/lifecycle/GameSessionWorldMaintenanceService.h"

#include "game/scenario/runtime/ScenarioDefinition.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/runtime/ObjectSimulationDamageDetail.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/movement/ObjectFootprintEvacuation.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/economy/ObjectEnergy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine {
using namespace object_lifecycle_detail;

namespace {

using Fixed = math::q32_32;

constexpr Fixed kConstructionEvacuationClearance{int32_t{40}};

void consumeGroupPathOptimization(ObjectOrderIntent& order) noexcept {
    if (order.groupPathId != 0 && order.hasTargetPosition) {
        order.targetX += order.groupPathOffsetX;
        order.targetY += order.groupPathOffsetY;
    }
    order.groupPathId = 0;
    order.groupPathMemberOrdinal = 0;
    order.groupPathMemberCount = 0;
    order.groupPathStartX = {};
    order.groupPathStartY = {};
    order.groupPathStartZ = {};
    order.groupPathOffsetX = {};
    order.groupPathOffsetY = {};
}

[[nodiscard]] bool isConstructionEvacuationFor(
    const ObjectOrderIntent& order, ObjectId obstacle) noexcept {
    return order.kind == ObjectOrderKind::Move &&
        order.source == ObjectOrderSource::System &&
        order.moveRouteSubtype == ObjectMoveRouteSubtype::Direct &&
        order.systemPurpose ==
            ObjectOrderSystemPurpose::ConstructionEvacuation &&
        order.targetObject == obstacle &&
        order.systemPurposeInstance == obstacle.value;
}

} // namespace

GameSessionObjectLifecycleTransactions::GameSessionObjectLifecycleTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort barrier,
    GameSessionObjectEventState* objectEvents) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_barrier(barrier),
      m_objectEvents(objectEvents) {}

void GameSessionObjectLifecycleTransactions::refreshDerivedAggregates(
    uint64_t confirmedTick) {
    m_world.m_objectEnergy.update(
        m_world.m_registry, m_content.m_players, confirmedTick);
    m_world.m_objectSimulation.updateRadarProviders(
        m_world.m_registry, m_world.m_objects, m_content.m_players,
        confirmedTick);
}

void GameSessionObjectLifecycleTransactions::spawnInitialContainmentPayloads(
    ObjectId host, uint32_t initialPathfindLayer, uint64_t confirmedTick) {
    if (!host || m_world.m_initialContainmentSpawnDepth >= 32u) return;
    const std::optional<ecs::entity> hostEntity =
        m_world.m_objects.entityFromId(host);
    if (!hostEntity) return;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(
            m_world.m_registry, *hostEntity);
    if (!runtime || !runtime->plan || runtime->initialPayloadsCreated) return;

    struct DepthGuard final {
        uint32_t& depth;
        explicit DepthGuard(uint32_t& value) : depth(value) { ++depth; }
        ~DepthGuard() { --depth; }
    } depthGuard{m_world.m_initialContainmentSpawnDepth};

    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *hostEntity);
    const PrimaryTeamComponent* team = ecs::try_get<PrimaryTeamComponent>(
        m_world.m_registry, *hostEntity);
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(
            m_world.m_registry, *hostEntity);
    if (!owner || !owner->player || !team || !team->team || !transform) return;
    const std::optional<ObjectTeamId> defaultTeam =
        m_world.m_objectTeams.defaultTeam(owner->player);
    if (!defaultTeam) return;

    const container::SharedPtr<const ObjectContainmentPlan> plan = runtime->plan;
    runtime->initialPayloadsCreated = true;
    for (const ObjectContainmentRule& rule : plan->rules) {
        const bool structuralPayload =
            rule.kind == ObjectContainmentKind::Overlord ||
            rule.kind == ObjectContainmentKind::Helix;
        const ObjectTeamId payloadTeam = structuralPayload
            ? team->team : *defaultTeam;
        for (const container::String& payloadTemplate :
             rule.payloadTemplateNames) {
            GameSessionObjectSpawnResult payload = spawnObject({
                .templateName = payloadTemplate,
                .owner = owner->player,
                .primaryTeam = payloadTeam,
                .transform = *transform,
                .initialPathfindLayer = initialPathfindLayer,
                .origin = ObjectCreationOrigin::System,
                .confirmedTick = confirmedTick,
                .producer = host,
            });
            const bool attached = payload &&
                m_world.m_objectSimulation.requestContainment(
                    m_world.m_registry, m_world.m_objects,
                    {.kind = ObjectContainmentRequestKind::Attach,
                     .container = host,
                     .object = payload.object,
                     .confirmedTick = confirmedTick,
                     .force = structuralPayload},
                    &m_content.m_players, &m_content.m_contentSnapshot);
            if (payload && !attached) {
                static_cast<void>(requestDestroyObject(
                    payload.object, ObjectDestroyReason::System,
                    confirmedTick));
            }
        }
    }
}

size_t GameSessionObjectLifecycleTransactions::evacuateConstructionFootprint(
    ObjectId structure, ObjectId builder, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !structure ||
        m_world.m_objects.isPendingDestroy(structure)) {
        return 0;
    }
    const std::optional<ecs::entity> structureEntity =
        m_world.m_objects.entityFromId(structure);
    if (!structureEntity) return 0;
    const ObjectGeometryComponent* structureGeometry =
        ecs::try_get<ObjectGeometryComponent>(
            m_world.m_registry, *structureEntity);
    const TransformComponent* structureTransform =
        ecs::try_get<TransformComponent>(
            m_world.m_registry, *structureEntity);
    const OwnerComponent* structureOwner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *structureEntity);
    if (!structureGeometry || !structureTransform || !structureOwner ||
        !structureOwner->player) {
        return 0;
    }
    const LogicFixedVec3 structurePosition = readAuthoritativeObjectPosition(
        m_world.m_registry, *structureEntity, *structureTransform);
    const Fixed structureYaw = readAuthoritativeObjectYaw(
        m_world.m_registry, *structureEntity, *structureTransform);
    const ObjectTerrainLayerComponent* structureLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(
            m_world.m_registry, *structureEntity);

    container::Vector<ObjectId> candidates;
    const auto identityView = ecs::view<const ObjectIdentityComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : identityView) {
        const ObjectId object = identityView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (object && object != structure && object != builder)
            candidates.push_back(object);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());

    size_t evacuated = 0;
    for (const ObjectId candidate : candidates) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(candidate);
        if (!entity || m_world.m_objects.isPendingDestroy(candidate) ||
            !m_barrier.objectAIOwnsMoveStop(candidate)) {
            continue;
        }
        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(
                m_world.m_registry, *entity);
        const TransformComponent* transform = ecs::try_get<TransformComponent>(
            m_world.m_registry, *entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(
                m_world.m_registry, *entity);
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *entity);
        const ObjectKindOfComponent* kinds = ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, *entity);
        const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(
            m_world.m_registry, *entity);
        const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(
            m_world.m_registry, *entity);
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(
                m_world.m_registry, *entity);
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(
                m_world.m_registry, *entity);
        const ObjectTerrainLayerComponent* layer =
            ecs::try_get<ObjectTerrainLayerComponent>(
                m_world.m_registry, *entity);
        if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive ||
            !transform || !geometry || !locomotion ||
            (health && health->effectivelyDead) ||
            (contained && contained->enclosing) ||
            (mapStatus && mapStatus->offMap) ||
            (structureLayer && layer &&
             structureLayer->pathfindLayer != layer->pathfindLayer) ||
            hasObjectKind(kinds, game::ObjectKindOf::Immobile) ||
            hasObjectKind(kinds, game::ObjectKindOf::Structure) ||
            hasObjectKind(kinds, game::ObjectKindOf::Aircraft) ||
            (status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::NoCollisions) |
                game::objectStatusBit(game::ObjectStatusFlag::Immobile))) ||
            relationshipBetweenPlayerAndObject(
                m_world.m_registry, m_content.m_players,
                structureOwner->player, *entity) == PlayerRelationship::Enemies) {
            continue;
        }

        const LogicFixedVec3 subjectPosition = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        std::optional<LogicFixedVec3> target =
            objectFootprintEvacuationTarget(
                structurePosition, structureYaw, *structureGeometry,
                subjectPosition, *geometry, candidate,
                kConstructionEvacuationClearance);
        if (!target) continue;
        const game::terrain::TerrainPathfindLayerId evacuationLayer =
            structureLayer ? structureLayer->pathfindLayer
                           : (layer ? layer->pathfindLayer
                                    : game::terrain::kGroundPathfindLayer);
        target->z = Fixed::from_raw(
            m_content.m_terrain.pathfindLayerHeightRawAt(
                evacuationLayer, target->x.raw(), target->y.raw())
                .value_or(m_content.m_terrain.groundHeightRaw(
                    target->x.raw(), target->y.raw())));

        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                m_world.m_registry, *entity);
        }
        if (!queue->orders.empty() &&
            isConstructionEvacuationFor(queue->orders.front(), structure)) {
            continue;
        }
        // Construction evacuation is an immediate system correction, like
        // RefCode's aiMoveToPositionEvenIfSleeping(). It must not silently
        // disappear merely because the unit already has a full waypoint
        // queue; preserve the imminent orders and discard only the farthest
        // future tail entry when capacity is exhausted.
        if (queue->orders.size() >=
            ObjectOrderQueueComponent::MaximumQueuedOrders)
            queue->orders.pop_back();
        if (!queue->orders.empty())
            consumeGroupPathOptimization(queue->orders.front());
        queue->orders.insert(queue->orders.begin(), {
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::System,
            .contextPlayer = structureOwner->player,
            .issuedTick = confirmedTick,
            .sourceSequence = structure.value,
            .targetObject = structure,
            .targetX = target->x,
            .targetY = target->y,
            .targetZ = target->z,
            .hasTargetPosition = true,
            .moveRouteSubtype = ObjectMoveRouteSubtype::Direct,
            .systemPurpose =
                ObjectOrderSystemPurpose::ConstructionEvacuation,
            .systemPurposeInstance = structure.value,
        });
        ++queue->revision;
        ++evacuated;
    }
    return evacuated;
}

GameSessionObjectSpawnResult GameSessionObjectLifecycleTransactions::spawnObject(ObjectSpawnRequest request) {
    GameSessionObjectSpawnResult result;
    result.scriptNameRequested = !request.scriptName.empty();
    // Legacy OCL/script payloads use NONE as an authored empty object slot.
    // Most typed adapters discard it earlier, but the authoritative creation
    // boundary must retain the same contract for every producer.  A genuine
    // unknown template still reaches the frozen-archetype diagnostic below;
    // only the explicit empty sentinel is a silent no-op.
    if (request.templateName.empty() ||
        container::asciiEqualIgnoreCase(request.templateName, "NONE")) {
        return result;
    }
    if (!m_content.m_active || !m_barrier) {
        TD_LOG_WARN("[GameSession] Cannot create object '{}' before session start", request.templateName);
        return result;
    }
    if (!m_content.m_players.get(request.owner)) {
        TD_LOG_WARN("[GameSession] Cannot create object '{}', owner {} is not materialized in this session",
                    request.templateName, request.owner.value);
        return result;
    }

    // ScriptActions::doCreateObject checks the old name before it touches a
    // Team/template. Preserve that special opt-in policy here, at the one
    // authoritative creation path: a live Object rejects creation, while an
    // effectively dead predecessor allows a later transferName() after the
    // new entity has been fully initialized. Ordinary map/production callers
    // keep their historical bind-only behavior.
    if (request.inheritScriptNamesFrom && !request.scriptName.empty()) {
        TD_LOG_WARN(
            "[GameSession] Object creation cannot both inherit aliases and bind an explicit script name");
        return result;
    }
    bool transferEffectivelyDeadScriptName = false;
    if (request.replaceEffectivelyDeadScriptName && !request.scriptName.empty()) {
        if (const std::optional<ObjectId> existing =
                m_presentation.m_scriptObjects.liveNamedObject(request.scriptName)) {
            if (!m_barrier.canCreateScriptObjectNamed(request.scriptName)) {
                TD_LOG_WARN("[GameSession] Duplicate live script objectName '{}' rejected before creating '{}'",
                             request.scriptName, request.templateName);
                return result;
            }
            transferEffectivelyDeadScriptName = true;
        }
    }
    if (!request.primaryTeam) {
        const std::optional<ObjectTeamId> defaultTeam = m_world.m_objectTeams.defaultTeam(request.owner);
        if (!defaultTeam) {
            TD_LOG_ERROR("[GameSession] Cannot create object '{}': owner {} has no default ObjectTeam",
                         request.templateName, request.owner.value);
            return result;
        }
        request.primaryTeam = *defaultTeam;
    }
    if (!m_world.m_objectTeams.isOwnedBy(request.primaryTeam, request.owner)) {
        TD_LOG_WARN("[GameSession] Cannot create object '{}': ObjectTeam {} is not controlled by player {}",
                    request.templateName, request.primaryTeam.value, request.owner.value);
        return result;
    }
    const container::SharedPtr<const game::ObjectArchetype> frozenArchetype =
        m_content.m_contentSnapshot.findObjectArchetype(request.templateName);
    if (!frozenArchetype) {
        TD_LOG_WARN("[GameSession] Cannot create object, compiled archetype is absent from this session's content snapshot: {}",
                    request.templateName);
        return result;
    }

    if (request.confirmedTick == 0) request.confirmedTick = m_presentation.m_confirmedTick;
    if (m_presentation.m_hasConfirmedFrame && request.confirmedTick != m_presentation.m_confirmedTick) {
        TD_LOG_WARN(
            "[GameSession] Cannot create object '{}' for tick {} while confirmed tick {} is active",
            request.templateName, request.confirmedTick, m_presentation.m_confirmedTick);
        return result;
    }
    // All map and script object creation flows converge here. Authoring
    // adapters have already quantized the pose; canonicalize its fixed yaw
    // without projecting it through float.
    request.transform.yawRadians = normalizeLegacyCreationOrientation(
        request.transform.yawRadians);
    // ObjectLifecycle may already hold unconsumed events from a prior
    // structural operation.  Capture its private suffix before creation so a
    // late script-name/Team invariant failure can roll back only this new
    // entity without consuming somebody else's events or flushing somebody
    // else's pending destroy.
    const size_t lifecycleEventCheckpoint = m_world.m_objects.eventCheckpoint();
    const ObjectSpawnResult spawned = m_world.m_objects.create(
        request, frozenArchetype->templateData, m_content.m_objectSimulationRules);
    result.object = spawned.object;
    result.entity = spawned.entity;
    if (!spawned) {
        TD_LOG_ERROR("[GameSession] Object lifecycle rejected creation of '{}'", request.templateName);
        return result;
    }
    // Attach only the typed, currently supported runtime components before
    // publishing creation. This keeps every observer from seeing a partially
    // initialized object while leaving unsupported locomotor appearances
    // explicit rather than simulating them with an incorrect fallback.
    ecs::get<ThingTemplateComponent>(m_world.m_registry, *spawned.entity).archetype = frozenArchetype;
    if (frozenArchetype->combatProfile) {
        ecs::emplace<ObjectCombatProfileComponent>(m_world.m_registry, *spawned.entity,
            ObjectCombatProfileComponent{.profile = frozenArchetype->combatProfile});
    }
    m_world.m_objectCombat.initializeObject(m_world.m_registry, *spawned.entity,
                                   *frozenArchetype, m_content.m_contentSnapshot,
                                   m_world.m_objectSimulation.rules().logicFramesPerSecond,
                                   request.confirmedTick);
    m_world.m_objectSimulation.initializeObject(m_world.m_registry, *spawned.entity,
                                        frozenArchetype->templateData, m_content.m_contentSnapshot,
                                        m_content.m_terrain, &m_content.m_simulationRandom);
    // ExperienceTracker exists on every RefCode Object, including
    // non-trainable buildings and helpers. It must precede UpgradeMux because
    // ExperienceScalarUpgrade may execute during the spawn activation pass.
    m_world.m_objectSimulation.initializeExperience(
        m_world.m_registry, *spawned.entity, frozenArchetype->templateData,
        request.confirmedTick);
    // ProductionUpdate's immutable plan is already attached through the
    // archetype.  Materialize only its per-factory queue/rally state before
    // publication, so no lifecycle listener can observe a half-assembled
    // producer.
    m_world.m_objectProduction.initializeObject(m_world.m_registry, *spawned.entity);
    // Energy recipe values are similarly immutable, while the player-wide
    // aggregate is rebuilt only after the complete spawn transaction below.
    m_world.m_objectEnergy.initializeObject(m_world.m_registry, *spawned.entity);
    const OwnerComponent* spawnedOwner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *spawned.entity);
    const PlayerState* ownerState = spawnedOwner ? m_content.m_players.get(spawnedOwner->player) : nullptr;
    const UpgradeMask ownerCompletedUpgrades = ownerState
        ? ownerState->upgrades.completed
        : UpgradeMask{};
    // Materialize every UpgradeMux consumer now, but defer owner-upgrade
    // activation until CreateModule::onCreate has had its source-defined
    // chance to add grants or conflicts. StartsActive state remains part of
    // each consumer's structural initialization.
    m_world.m_objectSimulation.initializeAutoHeal(
        m_world.m_registry, *spawned.entity, {}, m_content.m_simulationRandom,
        request.confirmedTick);
    m_world.m_objectSimulation.initializeDeathReactionRuntime(
        m_world.m_registry, *spawned.entity, {});
    m_world.m_objectSimulation.materializeObjectUpgrades(
        m_world.m_registry, *spawned.entity);
    const auto abortUnpublishedSpawn = [&]() {
        // This is a targeted transaction abort, not a normal lifecycle
        // destroy.  It must leave pre-existing lifecycle events and pending
        // destroys untouched for their original phase boundary.
        m_world.m_objectTeams.removeObject(spawned.object);
        if (!m_world.m_objects.abortUnpublishedCreate(spawned.object, lifecycleEventCheckpoint)) {
            TD_LOG_ERROR("[GameSession] Failed to abort unpublished object {} after creation transaction failure",
                         spawned.object.value);
        }
        result.object = INVALID_OBJECT_ID;
        result.entity.reset();
        result.scriptNameBound = false;
    };
    // The team request was validated before allocation. A failure here is an
    // invariant violation (normally impossible for a newly issued ObjectId);
    // abandon only the newly-created entity rather than exposing an
    // OwnerComponent and primary-team index that disagree.
    if (!m_world.m_objectTeams.assignObject(request.primaryTeam, spawned.object)) {
        TD_LOG_ERROR("[GameSession] ObjectTeam assignment failed for newly created object {}", spawned.object.value);
        abortUnpublishedSpawn();
        return result;
    }
    uint32_t inheritedPrioritySetId = 0;
    if (const auto inheritedPriority =
            m_world.m_objectTeams.attackPrioritySet(request.primaryTeam)) {
        const auto inheritedSet = m_presentation.m_scriptAttackPrioritySets.find(
            container::String{*inheritedPriority});
        if (inheritedSet != m_presentation.m_scriptAttackPrioritySets.end()) {
            inheritedPrioritySetId = inheritedSet->second.id;
        }
    }
    ObjectAIAttitude inheritedAttitude = ObjectAIAttitude::Normal;
    if (m_presentation.m_scenarioDefinition) {
        for (const scenario::ScriptTeamDefinition& definition :
             m_presentation.m_scenarioDefinition->scriptTeams()) {
            const container::Span<const ObjectTeamId> instances =
                m_world.m_objectTeams.scenarioTeamInstances(
                    definition.id);
            if (std::find(instances.begin(), instances.end(),
                          request.primaryTeam) == instances.end()) {
                continue;
            }
            inheritedAttitude = static_cast<ObjectAIAttitude>(
                std::clamp(definition.plan.initialAttitude, -2, 2));
            break;
        }
    }
    if (frozenArchetype->hasAiUpdate &&
        (inheritedPrioritySetId != 0 ||
         inheritedAttitude != ObjectAIAttitude::Normal)) {
        ecs::emplace<ObjectAIBehaviorPolicyComponent>(
            m_world.m_registry, *spawned.entity,
            ObjectAIBehaviorPolicyComponent{
                .attackPrioritySetId = inheritedPrioritySetId,
                .attitude = inheritedAttitude,
            });
    }
    if (const auto relationshipPolicy =
            m_world.m_objectTeams.relationshipPolicy(request.primaryTeam)) {
        ecs::emplace<ObjectRelationshipOverrideComponent>(
            m_world.m_registry, *spawned.entity,
            ObjectRelationshipOverrideComponent{
                .policy = relationshipPolicy,
            });
    }
    if (request.inheritScriptNamesFrom) {
        static_cast<void>(m_presentation.m_scriptObjects.transferObjectNames(
            request.inheritScriptNamesFrom, spawned.object));
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
        static_cast<void>(m_presentation.m_scriptObjectPresentation.transferCustomIndicatorColor(
            request.inheritScriptNamesFrom, spawned.object,
            {.presentationEpoch = m_presentation.m_scriptPresentationEpoch,
             .sequence = m_presentation.m_scriptPresentationSequence,
             .confirmedTick = request.confirmedTick,
             .sourceScriptId = 0,
             .ordinal = 0}));
    }
    if (!request.scriptName.empty()) {
        // RefCode's transferObjectName copies the old Object's custom
        // indicator colour into its replacement. Capture the stable ID before
        // the name index switches to the new object.
        const std::optional<ObjectId> replacedObject = transferEffectivelyDeadScriptName
            ? m_presentation.m_scriptObjects.liveNamedObject(request.scriptName) : std::nullopt;
        result.scriptNameBound = transferEffectivelyDeadScriptName
            ? m_presentation.m_scriptObjects.transferName(request.scriptName, spawned.object)
            : m_presentation.m_scriptObjects.bindName(request.scriptName, spawned.object);
        if (!result.scriptNameBound) {
            if (request.replaceEffectivelyDeadScriptName) {
                // This path should be unreachable after the preflight above,
                // but Script creation must be atomic with its requested name:
                // never publish an extra unbound Object if an index invariant
                // is violated between validation and binding.
                abortUnpublishedSpawn();
                return result;
            }
        }
        if (result.scriptNameBound && replacedObject) {
            ++m_presentation.m_scriptPresentationSequence;
            if (m_presentation.m_scriptPresentationSequence == 0) ++m_presentation.m_scriptPresentationSequence;
            static_cast<void>(m_presentation.m_scriptObjectPresentation.transferCustomIndicatorColor(
                *replacedObject, spawned.object,
                {.presentationEpoch = m_presentation.m_scriptPresentationEpoch,
                 .sequence = m_presentation.m_scriptPresentationSequence,
                 .confirmedTick = request.confirmedTick,
                 .sourceScriptId = 0,
                 .ordinal = 0}));
            // Unlike custom indicator colour, topple direction belongs to
            // the durable script name.  Remove the projection from the old
            // object and apply it to whichever object now owns that alias.
            if (const std::optional<ecs::entity> replacedEntity =
                    m_world.m_objects.entityFromIdIncludingPending(*replacedObject)) {
                ecs::remove<ObjectScriptToppleDirectionComponent>(
                    m_world.m_registry, *replacedEntity);
            }
        }
        if (result.scriptNameBound) {
            const auto overrideDirection =
                m_presentation.m_scriptToppleDirections.find(request.scriptName);
            if (overrideDirection != m_presentation.m_scriptToppleDirections.end()) {
                ObjectScriptToppleDirectionComponent* component =
                    ecs::try_get<ObjectScriptToppleDirectionComponent>(
                        m_world.m_registry, *spawned.entity);
                if (!component) {
                    ecs::emplace<ObjectScriptToppleDirectionComponent>(
                        m_world.m_registry, *spawned.entity,
                        ObjectScriptToppleDirectionComponent{
                            .direction = overrideDirection->second,
                            .revision = 1,
                        });
                } else {
                    component->direction = overrideDirection->second;
                    ++component->revision;
                }
            }
        }
    }
    const ObjectKindOfComponent* placementKinds =
        ecs::try_get<ObjectKindOfComponent>(
            m_world.m_registry, *spawned.entity);
    const bool structurePlacement =
        hasObjectKind(placementKinds, game::ObjectKindOf::Structure) ||
        frozenArchetype->templateData.body.kind ==
            game::ObjectBodyKind::Structure ||
        frozenArchetype->templateData.body.kind ==
            game::ObjectBodyKind::HiveStructure;
    // Dozer/Worker construction always flattens a non-small structure before
    // it is published to navigation.  Keep that invariant at the unified
    // lifecycle boundary so a caller cannot accidentally create an
    // under-construction site without the terrain transaction (LINEBUILD used
    // to do exactly that). The explicit flag remains available to OCL/script
    // structure spawns which are not construction sites.
    const bool flattenTerrainForStructure =
        request.flattenTerrainForStructure || request.startsUnderConstruction;
    if (structurePlacement && flattenTerrainForStructure &&
        m_content.m_terrain.isLoaded()) {
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(
                m_world.m_registry, *spawned.entity);
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, *spawned.entity);
        if (geometry && transform && transform->authoritative) {
            const ObjectFixedTransformComponent& flattenTransform =
                request.terrainFlattenPlacement
                ? request.terrainFlattenPlacement->footprintTransform
                : *transform;
            const uint64_t terrainRevisionBefore =
                m_content.m_terrain.map().revision();
            const game::terrain::TerrainFlattenResult flattenResult =
                m_content.m_terrain.flattenFootprintRaw({
                    .centerXRaw = flattenTransform.position.x.raw(),
                    .centerYRaw = flattenTransform.position.y.raw(),
                    .yawRadiansRaw = flattenTransform.yawRadians.raw(),
                    .majorRadiusRaw = geometry->majorRadiusFixed.raw(),
                    .minorRadiusRaw = geometry->minorRadiusFixed.raw(),
                    .shape = geometry->shape == ObjectGeometryShape::Box
                        ? game::terrain::TerrainFlattenShape::OrientedBox
                        : game::terrain::TerrainFlattenShape::Circle,
                    .isSmall = geometry->isSmall,
                });
            if (request.startsUnderConstruction) {
                TD_LOG_INFO(
                    "[GameSession] Construction terrain flatten: object={} template='{}' evaluated={} changed={} small={} shape={} centerRaw=({}, {}) radiiRaw=({}, {}) revision={}->{} heightRaw={}",
                    spawned.object.value, request.templateName,
                    flattenResult.evaluated, flattenResult.changed,
                    geometry->isSmall,
                    geometry->shape == ObjectGeometryShape::Box ? "box" : "circle",
                    flattenTransform.position.x.raw(),
                    flattenTransform.position.y.raw(),
                    geometry->majorRadiusFixed.raw(),
                    geometry->minorRadiusFixed.raw(),
                    terrainRevisionBefore,
                    m_content.m_terrain.map().revision(),
                    flattenResult.centerHeightRaw);
            }
            const bool adjustFinalObjectZ =
                !request.terrainFlattenPlacement ||
                request.terrainFlattenPlacement->adjustFinalObjectZ;
            if (adjustFinalObjectZ) {
                // Dozer/Worker and the standalone LIKE_EXISTING stage always
                // perform this snap even when GeometryIsSmall or no sample
                // needed lowering. RefCode's OCL samples the original `pos`,
                // not the offset chunk position.
                const math::q32_32 sampleX =
                    request.terrainFlattenPlacement
                    ? request.terrainFlattenPlacement->groundSampleX
                    : transform->position.x;
                const math::q32_32 sampleY =
                    request.terrainFlattenPlacement
                    ? request.terrainFlattenPlacement->groundSampleY
                    : transform->position.y;
                LogicFixedVec3 adjusted = transform->position;
                // A construction footprint has just selected one lowering
                // plane. Re-sampling the centre can land on a triangle pulled
                // below that plane by an existing low neighbour, which sinks
                // the building while other footprint vertices remain at the
                // plane and visibly enter its foundation. OCL's detached
                // LIKE_EXISTING path retains its authored sample semantics.
                adjusted.z = request.startsUnderConstruction &&
                        flattenResult.evaluated
                    ? math::q32_32::from_raw(
                          flattenResult.flattenedPlaneHeightRaw)
                    : math::q32_32::from_raw(
                          m_content.m_terrain.groundHeightRaw(
                              sampleX.raw(), sampleY.raw()));
                request.transform.position = adjusted;
                writeAuthoritativeObjectPosition(
                    m_world.m_registry,
                    *spawned.entity, adjusted);
            }
        }
    }
    if (request.startsUnderConstruction) {
        const game::ObjectGeometryTemplate& geometry =
            frozenArchetype->templateData.geometry;
        ClientTerrainConstructionFootprint footprint{
            .center = {
                request.transform.position.x.to_float(),
                request.transform.position.y.to_float(),
                request.transform.position.z.to_float(),
            },
            .yawRadians = request.transform.yawRadians.to_float(),
            .halfExtentX = geometry.majorRadiusFixed.to_float(),
            .halfExtentY = geometry.minorRadiusFixed.to_float(),
            .radius = geometry.boundingCircleRadiusFixed.to_float(),
            .height = geometry.heightFixed.to_float(),
            .orientedBox =
                geometry.type == game::ObjectGeometryType::Box,
        };
        // BuildAssistant clears even non-collidable buffered decoration after
        // placement succeeds.  This point is past every abortable Team/name
        // invariant, but still precedes lifecycle publication and onCreate.
        static_cast<void>(
            m_world.m_clientTerrainObjects.removeForConstruction(footprint));
    }
    // Object construction in RefCode asks the controlling Player for the
    // template's ProductionVeterancyLevel before publishing the Object. The
    // unified spawn boundary deliberately applies this to map/script/system
    // creation too, matching that constructor semantics rather than limiting
    // it to ObjectProductionSystem exits. Apply it only after every abortable
    // Team/name invariant has committed so an unpublished rollback cannot
    // leak a veterancy event for an entity that never became visible.
    if (ownerState) {
        const game::ObjectVeterancyLevel initialLevel = productionVeterancyLevel(
            *ownerState, frozenArchetype->templateData.name);
        if (initialLevel != game::ObjectVeterancyLevel::Regular) {
            static_cast<void>(m_world.m_objectSimulation.setObjectVeterancyLevel(
                m_world.m_registry, m_world.m_objects, spawned.object, initialLevel,
                ownerCompletedUpgrades, request.confirmedTick,
                {.players = &m_content.m_players,
                 .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
                 .content = &m_content.m_contentSnapshot,
                 .random = &m_content.m_simulationRandom,
                 .effects = &m_world.m_objectSimulation}));
        }
    }
    // Keep the source engine's distinct onCreate phase. It follows production
    // veterancy, but runs only after every abortable Team/name invariant has
    // committed so a rejected modern spawn cannot leak player upgrades.
    static_cast<void>(m_world.m_objectSimulation.onObjectCreated(
        m_world.m_registry, m_world.m_objects, m_world.m_ownership, spawned.object,
        ownerCompletedUpgrades, request.confirmedTick,
        {.players = &m_content.m_players,
         .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
         .content = &m_content.m_contentSnapshot,
         .random = &m_content.m_simulationRandom,
         .effects = &m_world.m_objectSimulation}));
    const OwnerComponent* postCreateOwner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *spawned.entity);
    const PlayerState* postCreateOwnerState = postCreateOwner
        ? m_content.m_players.get(postCreateOwner->player)
        : nullptr;
    const UpgradeMask postCreateCompletedUpgrades = postCreateOwnerState
            ? postCreateOwnerState->upgrades.completed
            : UpgradeMask{};
    m_world.m_objectSimulation.activateInitialObjectUpgrades(
        m_world.m_registry, m_world.m_objects, spawned.object, postCreateCompletedUpgrades,
        request.confirmedTick,
        {.players = &m_content.m_players,
         .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
         .content = &m_content.m_contentSnapshot,
         .random = &m_content.m_simulationRandom,
         .terrain = &m_content.m_terrain,
         .effects = &m_world.m_objectSimulation});
    // RefCode Object::initObject applies the ScriptEngine-owned global
    // difficulty policy after owner Upgrade modules have initialized and
    // before the creation event becomes visible. Future objects therefore
    // inherit the latest OBJECT_ALLOW_BONUSES value without a script scan.
    static_cast<void>(m_barrier.applyObjectDifficultyBonusPolicy(
        spawned.object, m_presentation.m_objectsReceiveDifficultyBonuses,
        request.confirmedTick));
    if (const std::optional<MapObjectInstanceOverrides>& mapOverrides =
            request.mapInstanceOverrides) {
        // ZH applies Object Panel values after ThingFactory::newObject has
        // completed initObject/onCreate and owner/template upgrades, but
        // before CreateModule::onBuildComplete. This must not be folded into
        // ObjectLifecycle's generic construction-time health path: modules
        // such as SupplyWarehouse and BoneFX observe pristine template state
        // during their own initialization.
        ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *spawned.entity);
        if (health && health->acceptsDamage &&
            (mapOverrides->maximumHealth ||
             mapOverrides->initialHealthFraction)) {
            using HealthScalar = ObjectHealthComponent::Scalar;
            if (mapOverrides->maximumHealth &&
                *mapOverrides->maximumHealth >= HealthScalar{}) {
                health->maximumFixed = *mapOverrides->maximumHealth;
                health->initialFixed = health->maximumFixed;
                if (health->currentFixed > health->maximumFixed) {
                    health->currentFixed = health->maximumFixed;
                }
            }
            if (mapOverrides->initialHealthFraction) {
                health->currentFixed = std::clamp(
                    health->initialFixed *
                        *mapOverrides->initialHealthFraction,
                    HealthScalar{}, health->maximumFixed);
            }
            health->previousFixed = health->currentFixed;
            health->damageState =
                object_simulation_detail::damageStateFor(
                    health->currentFixed, health->maximumFixed,
                    m_content.m_objectSimulationRules);
            health->effectivelyDead =
                health->currentFixed <= HealthScalar{};
            if (RenderModelComponent* visual =
                    ecs::try_get<RenderModelComponent>(
                        m_world.m_registry,
                        *spawned.entity)) {
                object_simulation_detail::projectBodyDamageVisual(
                    objectBodyDamagePresentationState(
                        m_world.m_registry, *spawned.entity,
                        health->damageState),
                    *visual);
            }
            if (health->damageState == ObjectBodyDamageState::Rubble) {
                object_simulation_detail::applyStructureRubbleGameplayState(
                    m_world.m_registry,
                    *spawned.entity,
                    m_content.m_objectSimulationRules,
                    request.confirmedTick);
            }
            markObjectDirty(
                m_world.m_registry, *spawned.entity,
                ObjectDirtyDomain::RenderExtraction);
        }

        if (mapOverrides->aggressiveness && frozenArchetype->hasAiUpdate) {
            ObjectAIBehaviorPolicyComponent* policy =
                ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            if (!policy) {
                policy = &ecs::emplace<ObjectAIBehaviorPolicyComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            }
            if (policy->attitude != *mapOverrides->aggressiveness) {
                policy->attitude = *mapOverrides->aggressiveness;
                ++policy->revision;
            }
        }
        if (mapOverrides->stoppingDistance) {
            if (ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        m_world.m_registry,
                        *spawned.entity)) {
                locomotion->closeEnough = *mapOverrides->stoppingDistance;
            }
        }
        if (mapOverrides->visionRange ||
            mapOverrides->shroudClearingRange) {
            const math::q32_32 vision = mapOverrides->visionRange.value_or(
                effectiveObjectVisionRangeFixed(
                    m_world.m_registry,
                    *spawned.entity));
            const math::q32_32 shroud =
                mapOverrides->shroudClearingRange.value_or(
                    effectiveObjectShroudClearingRangeFixed(
                        m_world.m_registry,
                        *spawned.entity));
            setObjectVisionRangeOverride(
                m_world.m_registry,
                *spawned.entity, vision, shroud);
        }
        if (mapOverrides->night || mapOverrides->snow) {
            ObjectEnvironmentModelConditionOverrideComponent* environment =
                ecs::try_get<
                    ObjectEnvironmentModelConditionOverrideComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            if (!environment) {
                environment = &ecs::emplace<
                    ObjectEnvironmentModelConditionOverrideComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            }
            if (mapOverrides->night) {
                environment->night = *mapOverrides->night ? 1 : 0;
            }
            if (mapOverrides->snow) {
                environment->snow = *mapOverrides->snow ? 1 : 0;
            }
            if (RenderModelComponent* visual =
                    ecs::try_get<RenderModelComponent>(
                        m_world.m_registry,
                        *spawned.entity)) {
                if (mapOverrides->night) {
                    visual->modelConditionFlags.set(
                        game::ModelConditionFlag::Night,
                        *mapOverrides->night);
                }
                if (mapOverrides->snow) {
                    visual->modelConditionFlags.set(
                        game::ModelConditionFlag::Snow,
                        *mapOverrides->snow);
                }
            }
            markObjectDirty(
                m_world.m_registry, *spawned.entity,
                objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        }
        if (mapOverrides->ambientSound ||
            mapOverrides->ambientSoundEnabled ||
            mapOverrides->ambientSoundLooping ||
            mapOverrides->ambientSoundLoopCount ||
            mapOverrides->ambientSoundMinVolume ||
            mapOverrides->ambientSoundVolume ||
            mapOverrides->ambientSoundMinRange ||
            mapOverrides->ambientSoundMaxRange ||
            mapOverrides->ambientSoundPriority) {
            ObjectAmbientAudioOverrideComponent overrideValue{
                .eventName = mapOverrides->ambientSound,
                .enabled = mapOverrides->ambientSoundEnabled,
                .looping = mapOverrides->ambientSoundLooping,
                .loopCount = mapOverrides->ambientSoundLoopCount,
                .minimumVolume = mapOverrides->ambientSoundMinVolume,
                .volume = mapOverrides->ambientSoundVolume,
                .minimumRange = mapOverrides->ambientSoundMinRange,
                .maximumRange = mapOverrides->ambientSoundMaxRange,
                .priority = mapOverrides->ambientSoundPriority,
            };
            if (overrideValue.eventName &&
                overrideValue.eventName->empty()) {
                overrideValue.enabled = false;
            }
            ecs::emplace<ObjectAmbientAudioOverrideComponent>(
                m_world.m_registry,
                *spawned.entity, std::move(overrideValue));
        }

        const auto setDisabled = [&](ObjectDisabledReason reason,
                                     const std::optional<bool>& enabled) {
            if (!enabled) return;
            if (*enabled) {
                static_cast<void>(ObjectDisabledSystem::clear(
                    m_world.m_registry, *spawned.entity,
                    reason, request.confirmedTick));
            } else {
                static_cast<void>(ObjectDisabledSystem::setUntil(
                    m_world.m_registry, *spawned.entity,
                    reason, OBJECT_DISABLED_FOREVER_TICK,
                    request.confirmedTick));
            }
        };
        setDisabled(
            ObjectDisabledReason::ScriptDisabled, mapOverrides->enabled);
        setDisabled(
            ObjectDisabledReason::ScriptUnderpowered,
            mapOverrides->powered);

        if (mapOverrides->indestructible && health &&
            health->acceptsDamage) {
            health->indestructible = *mapOverrides->indestructible;
        }
        if (mapOverrides->selectable) {
            const game::ObjectStatusMask unselectable =
                game::objectStatusBit(game::ObjectStatusFlag::Unselectable);
            static_cast<void>(ObjectStatusSystem::apply(
                m_world.m_registry, *spawned.entity,
                {.setMask = *mapOverrides->selectable ? 0 : unselectable,
                 .clearMask = *mapOverrides->selectable ? unselectable : 0,
                 .confirmedTick = request.confirmedTick}));
        }
        const bool hasPanelPolicy = mapOverrides->unsellable ||
            mapOverrides->aiRecruitable ||
            mapOverrides->playerTargetable;
        if (hasPanelPolicy) {
            ObjectScriptPanelPolicyComponent* policy =
                ecs::try_get<ObjectScriptPanelPolicyComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            if (!policy) {
                policy = &ecs::emplace<ObjectScriptPanelPolicyComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            }
            if (mapOverrides->unsellable) {
                policy->unsellable = *mapOverrides->unsellable;
            }
            if (mapOverrides->aiRecruitable &&
                frozenArchetype->hasAiUpdate) {
                policy->aiRecruitable = *mapOverrides->aiRecruitable;
            }
            if (mapOverrides->playerTargetable) {
                policy->playerTargetable =
                    *mapOverrides->playerTargetable;
            }
            ++policy->revision;
        }

        const ObjectUpgradeExecutionContext mapUpgradeContext{
            .players = &m_content.m_players,
            .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
            .content = &m_content.m_contentSnapshot,
            .random = &m_content.m_simulationRandom,
            .terrain = &m_content.m_terrain,
            .effects = &m_world.m_objectSimulation,
        };
        if (mapOverrides->veterancy) {
            const ObjectExperienceComponent* experience =
                ecs::try_get<ObjectExperienceComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            if (experience && experience->trainable) {
                static_cast<void>(m_world.m_objectSimulation.
                    setObjectVeterancyLevel(
                        m_world.m_registry,
                        m_world.m_objects,
                        spawned.object, *mapOverrides->veterancy,
                        postCreateCompletedUpgrades,
                        request.confirmedTick, mapUpgradeContext));
            }
        }
        for (const container::String& upgrade :
             mapOverrides->grantedUpgrades) {
            if (upgrade.empty()) continue;
            const UpgradeCatalog* upgradeCatalog =
                m_content.m_contentSnapshot.upgradeCatalog();
            const UpgradeDefinition* definition = upgradeCatalog
                ? upgradeCatalog->find(upgrade) : nullptr;
            if (!definition) continue;
            static_cast<void>(m_world.m_objectSimulation.
                completeObjectUpgrade(
                    m_world.m_registry,
                    m_world.m_objects,
                    spawned.object, definition->id,
                    postCreateCompletedUpgrades,
                    request.confirmedTick, mapUpgradeContext));
        }
    }
    const ObjectStatusComponent* buildStatus =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *spawned.entity);
    const bool waitsForConstruction = buildStatus && buildStatus->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    if (!waitsForConstruction) {
        static_cast<void>(m_world.m_objectSimulation.onObjectBuildCompleted(
            m_world.m_registry, m_world.m_objects, m_world.m_ownership, spawned.object,
            postCreateCompletedUpgrades, request.confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
             .content = &m_content.m_contentSnapshot,
             .random = &m_content.m_simulationRandom,
             .effects = &m_world.m_objectSimulation}));
        if (request.scoreAsBuilt && postCreateOwner) {
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                     *spawned.entity);
            if (const std::optional<PlayerScoredObjectKind> kind =
                    builtScoreKind(kinds)) {
                static_cast<void>(m_content.m_players.recordObjectBuilt(
                    postCreateOwner->player,
                    frozenArchetype->templateData.name, *kind));
            }
        }
        if (request.academyAsProduction && postCreateOwner) {
            recordAcademyProductionForObject(
                m_content.m_players, m_world.m_registry, *spawned.entity,
                postCreateOwner->player, request.confirmedTick,
                static_cast<uint32_t>(std::max(
                    1, m_content.m_startInfo.gameSpeedFPS)));
        }
        if (request.scoreConstructionCost && postCreateOwnerState) {
            const int64_t buildCost = std::max<int64_t>(
                0, calculateObjectBuildCost(
                    *frozenArchetype, *postCreateOwnerState,
                    m_world.m_registry, m_world.m_objects));
            if (buildCost > 0) {
                static_cast<void>(m_content.m_players.recordMoneySpent(
                    postCreateOwner->player,
                    static_cast<uint64_t>(buildCost)));
            }
        }
    }
    const ObjectKindOfComponent* spawnedKinds =
        ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *spawned.entity);
    if (hasObjectKind(spawnedKinds, game::ObjectKindOf::Mine) ||
        hasObjectKind(spawnedKinds, game::ObjectKindOf::BoobyTrap) ||
        hasObjectKind(spawnedKinds, game::ObjectKindOf::Demotrap)) {
        static_cast<void>(m_content.m_players.recordAcademyEvent(
            NEUTRAL_PLAYER_ID, PlayerAcademyEvent::MineCreated));
    }
    // At this point no later creation invariant can abort the entity. Refresh
    // immediately so same-tick script/query consumers observe a newly built
    // power producer or consumer without waiting for the next world update.
    refreshDerivedAggregates(request.confirmedTick);
    static_cast<void>(m_barrier.consumeObjectLifecycleEvents());
    // InitialRoster/InitialPayload belongs to the module creation boundary,
    // including structures which are still under construction.  This also
    // centralizes the legacy default-Team versus host-Team distinction and
    // the one-shot latch used by completion/rebuild paths.
    spawnInitialContainmentPayloads(
        spawned.object,
        request.initialPathfindLayer.value_or(
            game::terrain::kGroundPathfindLayer),
        request.confirmedTick);
    // Direct map/script/production spawns must observe onCreate UpgradeMux
    // effects synchronously. Calls made from the existing work stack are
    // reentrancy-guarded and collected by that stack at its next boundary.
    m_barrier.drainGameplayTransactions();
    return result;
}

bool GameSessionObjectLifecycleTransactions::completeConstruction(
    ObjectId id, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !id ||
        m_world.m_objects.isPendingDestroy(id) || !m_objectEvents) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(id);
    if (!entity) return false;
    ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(
        m_world.m_registry, *entity);
    if (!status || !status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction))) {
        return false;
    }

    // Reserve the navigation mutation before changing any lifecycle state.
    // A rejected event must leave the object wholly under construction so the
    // causal stack can fault without publishing a completed building whose
    // footprint still has the old state.
    const bool blocksGround = session_navigation::blocksGround(
        m_world.m_registry, *entity);
    const bool blocksAir = session_navigation::blocksAircraft(
        m_world.m_registry, *entity);
    if ((blocksGround || blocksAir) &&
        !GameSessionNavigationTransactions{m_content, m_presentation}
             .submitBuildingState(
                 id, confirmedTick,
                 navigation::NavigationDynamicEventReason::CompletionStateChanged,
                 navigation::NavigationBuildingState::Complete,
                 blocksGround, blocksAir)) {
        TD_LOG_ERROR(
            "[GameSession] Failed to reserve navigation completion for object {} at tick {}",
            id.value, confirmedTick);
        return false;
    }
    const ObjectStatusTransition transition = ObjectStatusSystem::apply(
        m_world.m_registry, *entity,
        {.clearMask =
             game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
             game::objectStatusBit(game::ObjectStatusFlag::Reconstructing),
         .confirmedTick = confirmedTick});
    if (!transition.changed()) return false;

    // ActiveBody::evaluateVisualCondition runs immediately after RefCode
    // clears UNDER_CONSTRUCTION. Construction progress kept the logical Body
    // state current but deliberately suppressed Drawable damage conditions;
    // expose the final state exactly once at this lifecycle edge.
    if (const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity)) {
        if (RenderModelComponent* visual =
                ecs::try_get<RenderModelComponent>(
                    m_world.m_registry, *entity)) {
            object_simulation_detail::projectBodyDamageVisual(
                health->damageState, *visual);
            markObjectDirty(
                m_world.m_registry, *entity,
                objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        }
    }

    const ObjectConstructionSiteComponent* constructionSite =
        ecs::try_get<ObjectConstructionSiteComponent>(
            m_world.m_registry, *entity);
    const bool isRebuild = constructionSite && constructionSite->rebuild;
    const uint32_t sourceSideOrdinal = constructionSite
        ? constructionSite->sourceSideOrdinal : UINT32_MAX;
    const uint32_t sourceBuildListOrdinal = constructionSite
        ? constructionSite->sourceBuildListOrdinal : UINT32_MAX;
    ecs::remove<ObjectConstructionSiteComponent>(m_world.m_registry, *entity);

    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player =
        owner ? m_content.m_players.get(owner->player) : nullptr;
    const UpgradeMask completedUpgrades =
        player ? player->upgrades.completed : UpgradeMask{};
    m_world.m_objectSimulation.onObjectConstructionCompleted(
        m_world.m_registry, m_world.m_objects, m_world.m_ownership, id,
        completedUpgrades, confirmedTick,
        {.players = &m_content.m_players,
         .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
         .content = &m_content.m_contentSnapshot,
         .random = &m_content.m_simulationRandom,
         .terrain = &m_content.m_terrain,
         .effects = &m_world.m_objectSimulation});

    // Player::onStructureConstructionComplete announces every superweapon a
    // finished structure carries, to every observer, choosing the own / ally /
    // enemy line by relationship. It is deliberately outside the isRebuild
    // guard below: a rebuilt ScudStorm is announced again. This transaction
    // owns no publication port, so only the audio emission is deferred; the
    // observer-relative decision is made here, where RefCode makes it.
    const ObjectSpecialPowerComponent* completedPowers =
        ecs::try_get<ObjectSpecialPowerComponent>(m_world.m_registry, *entity);
    const PlayerState* superweaponObserver =
        m_content.m_players.localPlayer();
    if (owner && completedPowers && superweaponObserver) {
        const EvaSuperweaponAudience audience = evaSuperweaponAudience(
            superweaponObserver->id == owner->player,
            m_content.m_players.relationship(
                superweaponObserver->id, owner->player));
        for (const ObjectSpecialPowerRuntime& instance :
             completedPowers->instances) {
            const SpecialPowerDefinition* definition =
                m_content.m_contentSnapshot.findSpecialPower(instance.content);
            if (!definition) continue;
            const std::optional<audio::EvaEventType> evaType =
                evaSuperweaponEvent(
                    definition->specialPowerType,
                    EvaSuperweaponAnnouncement::Detected, audience);
            if (!evaType) continue;
            m_presentation.m_pendingEvaAnnouncements.push_back({
                .type = *evaType,
                .confirmedTick = confirmedTick,
                .variationKey =
                    (static_cast<uint64_t>(id.value) << 32u) ^
                    static_cast<uint64_t>(instance.content.value),
            });
        }
    }

    if (!isRebuild && owner && player) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        if (type && !type->name.empty()) {
            if (const std::optional<PlayerScoredObjectKind> kind =
                    builtScoreKind(kinds)) {
                static_cast<void>(m_content.m_players.recordObjectBuilt(
                    owner->player, type->name, *kind));
            }
        }
        if (type && type->archetype) {
            const int64_t buildCost = std::max<int64_t>(
                0, calculateObjectBuildCost(
                       *type->archetype, *player, m_world.m_registry,
                       m_world.m_objects));
            if (buildCost > 0) {
                static_cast<void>(m_content.m_players.recordMoneySpent(
                    owner->player, static_cast<uint64_t>(buildCost)));
            }
        }
        recordAcademyProductionForObject(
            m_content.m_players, m_world.m_registry, *entity, owner->player,
            confirmedTick, static_cast<uint32_t>(std::max(
                1, m_content.m_startInfo.gameSpeedFPS)));
    }

    if (sourceSideOrdinal != UINT32_MAX &&
        sourceBuildListOrdinal != UINT32_MAX &&
        m_presentation.m_scenarioDefinition) {
        const auto intents = m_presentation.m_scenarioDefinition->buildIntents();
        const auto intent = std::find_if(
            intents.begin(), intents.end(),
            [sourceSideOrdinal, sourceBuildListOrdinal](
                const scenario::ScenarioBuildIntent& candidate) noexcept {
                return candidate.sourceSideOrdinal == sourceSideOrdinal &&
                    candidate.sourceBuildListOrdinal == sourceBuildListOrdinal;
            });
        if (intent != intents.end()) {
            m_objectEvents->m_objectHookEvents.push_back({
                .sourceSideOrdinal = sourceSideOrdinal,
                .sourceBuildListOrdinal = sourceBuildListOrdinal,
                .object = id,
            });
        }
    }

    // PostFinalize performs the single authoritative aggregate pass after all
    // construction/destroy/EMP transactions and before Production.  Calling
    // it here made K same-frame completions perform K full Energy/Radar world
    // scans and then repeat the same scan in PostFinalize.
    return true;
}

bool GameSessionObjectLifecycleTransactions::requestDestroyObject(ObjectId id, ObjectDestroyReason reason,
                                       uint64_t confirmedTick) {
    if (!m_content.m_active || !m_barrier) return false;
    const bool requested = m_world.m_objects.requestDestroy(id, reason, confirmedTick);
    // ObjectLifecycle hides a pending-destroy entity immediately. Publish the
    // request now as well, so player/team/named-object indexes never continue
    // to advertise an unavailable ObjectId until the physical frame-end free.
    if (requested)
    {
        static_cast<void>(m_barrier.consumeObjectLifecycleEvents());
        // onDelete is a synchronous ZH callback boundary. Tactical special
        // object kills, cancelled rail containment and other gameplay suffixes
        // published by DestroyRequested must close before the caller advances.
        m_barrier.resolveQueuedObjectDamage();
    }
    return requested;
}

bool GameSessionObjectLifecycleTransactions::destroyObject(ObjectId id) {
    // Request-time publication already removes an ObjectId from named/team/
    // ownership queries, which is the required same-script visibility.  A
    // direct global flush here would also physically destroy unrelated
    // objects which were queued by combat or another script effect earlier
    // in this frame.  Reclamation stays at updatePostCommandSystems()'s
    // normal lifecycle boundary.
    return requestDestroyObject(id, ObjectDestroyReason::Script, m_presentation.m_confirmedTick);
}

size_t GameSessionObjectLifecycleTransactions::flushPending() {
    // Publish request edges before physical reclamation while pending entities
    // still own containment and persistent-effect state.
    static_cast<void>(m_barrier.consumeObjectLifecycleEvents());
    // Finish request-time gameplay while onDelete components still exist.
    m_barrier.resolveQueuedObjectDamage();
    const FrameCommitResult* commit = m_barrier.frameCommitResult();
    if (commit && commit->faulted()) return 0;

    m_world.m_objectProduction.cancelPendingDestroyed(
        m_world.m_registry, m_world.m_objects, m_content.m_players);
    const size_t destroyed = m_world.m_objects.flushRequestedDestroys();
    if (destroyed != 0) {
        m_world.m_spatialIndex.pruneMissing(m_world.m_objects);
    }
    // Publish Destroyed edges only after all reclaimed components are gone.
    static_cast<void>(m_barrier.consumeObjectLifecycleEvents());
    return destroyed;
}

} // namespace engine

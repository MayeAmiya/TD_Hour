#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/lifecycle/ObjectDeathWalk.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationDamageDetail.h"
#include "game/object/component/ObjectDirty.h"

namespace engine::object_simulation_detail {

namespace {

[[nodiscard]] ObjectDeathWalkState beginDeathWalk(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ecs::entity entity,
    ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar resolvedDamage, HealthScalar clippedDamage,
    ObjectUpgradeExecutionContext context, uint64_t sessionSeed,
    bool scoreTheKillPath) {
    health.effectivelyDead = true;
    health.terminalDeathIssued = true;
    markObjectDirty(registry, entity, kObjectDirtyAll);

    // ActiveBody calls damager->scoreTheKill(obj) before Object::onDie and the
    // Body executor has already committed those score mutations at this edge.
    // Freeze only the immutable publication facts now: authored Die callbacks
    // may transfer ownership/status or destroy the source before presentation
    // consumes the Died event.
    const OwnerComponent* scoreVictimOwner =
        ecs::try_get<OwnerComponent>(registry, entity);
    const PlayerId scoreVictimPlayer = scoreVictimOwner
        ? scoreVictimOwner->player : INVALID_PLAYER_ID;
    const PlayerState* scoreVictimPlayerState =
        context.players && scoreVictimPlayer
            ? context.players->get(scoreVictimPlayer)
            : nullptr;
    const ObjectKindOfComponent* scoreVictimKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    const ObjectStatusComponent* scoreVictimStatus =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const ThingTemplateComponent* scoreVictimTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const bool scoreTagged =
        hasKind(scoreVictimKinds, game::ObjectKindOf::Score) ||
        hasKind(scoreVictimKinds, game::ObjectKindOf::ScoreDestroy);
    ObjectHealthScoreKind healthScoreKind = ObjectHealthScoreKind::None;
    if (scoreTagged && hasKind(scoreVictimKinds,
                               game::ObjectKindOf::Structure)) {
        healthScoreKind = ObjectHealthScoreKind::Building;
    } else if (scoreTagged &&
               (hasKind(scoreVictimKinds, game::ObjectKindOf::Infantry) ||
                hasKind(scoreVictimKinds, game::ObjectKindOf::Vehicle))) {
        healthScoreKind = ObjectHealthScoreKind::Unit;
    }
    const container::String scoreVictimTemplateName = scoreVictimTemplate
        ? scoreVictimTemplate->name : container::String{};
    const bool scoreVictimPlayableSide =
        scoreVictimPlayerState && scoreVictimPlayerState->playableSide;
    const bool scoreVictimIgnoredInGui =
        hasKind(scoreVictimKinds, game::ObjectKindOf::IgnoredInGui);
    const bool scoreVictimUnderConstruction = scoreVictimStatus &&
        scoreVictimStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction));
    const bool victimEvaBuilding =
        hasKind(scoreVictimKinds, game::ObjectKindOf::Structure) &&
        hasKind(scoreVictimKinds, game::ObjectKindOf::MpCountForVictory);
    const bool victimEvaUnit =
        hasKind(scoreVictimKinds, game::ObjectKindOf::Infantry) ||
        hasKind(scoreVictimKinds, game::ObjectKindOf::Vehicle);
    const TransformComponent* scoreVictimTransform =
        ecs::try_get<TransformComponent>(registry, entity);
    const ObjectGeometryComponent* scoreVictimGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const LogicFixedVec3 scoreVictimPosition = scoreVictimTransform
        ? readAuthoritativeObjectPosition(
              registry, entity, *scoreVictimTransform)
        : LogicFixedVec3{};
    const std::optional<ecs::entity> scoreSourceEntity =
        lifecycle.entityFromIdIncludingPending(request.source);
    const OwnerComponent* scoreSourceOwner = scoreSourceEntity
        ? ecs::try_get<OwnerComponent>(registry, *scoreSourceEntity)
        : nullptr;
    const PlayerId scoreSourcePlayer = scoreSourceOwner
        ? scoreSourceOwner->player : INVALID_PLAYER_ID;
    const bool scoreSourceIsEnemy = context.players && scoreSourceEntity &&
        relationshipBetweenObjects(
            registry, *context.players, *scoreSourceEntity, entity) ==
            PlayerRelationship::Enemies;
    const TransformComponent* scoreSourceTransform = scoreSourceEntity
        ? ecs::try_get<TransformComponent>(registry, *scoreSourceEntity)
        : nullptr;
    const ObjectGeometryComponent* scoreSourceGeometry = scoreSourceEntity
        ? ecs::try_get<ObjectGeometryComponent>(registry, *scoreSourceEntity)
        : nullptr;
    const ObjectAirborneComponent* scoreSourceAirborne = scoreSourceEntity
        ? ecs::try_get<ObjectAirborneComponent>(registry, *scoreSourceEntity)
        : nullptr;
    const ObjectStatusComponent* scoreSourceStatus = scoreSourceEntity
        ? ecs::try_get<ObjectStatusComponent>(registry, *scoreSourceEntity)
        : nullptr;
    const LogicFixedVec3 scoreSourcePosition =
        scoreSourceEntity && scoreSourceTransform
            ? readAuthoritativeObjectPosition(
                  registry, *scoreSourceEntity, *scoreSourceTransform)
            : LogicFixedVec3{};

    const ObjectDeathReactionComponent* reactionComponent =
        ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectDeathReactionPlan> plan =
        reactionComponent ? reactionComponent->plan : nullptr;
    const ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);

    return {
        .damage = request,
        .plan = plan,
        .diedEvent = {
            .kind = ObjectHealthEventKind::Died,
            .object = request.target,
            .source = request.source,
            .damageType = request.damageType,
            .damageFxType = request.damageFxOverride.value_or(
                request.damageType),
            .deathType = request.deathType,
            .requestedAmount = request.amount,
            .previousHealth = health.previousFixed,
            .currentHealth = health.currentFixed,
            .previousState = health.damageState,
            .currentState = health.damageState,
            .confirmedTick = request.confirmedTick,
            .sourcePlayer = scoreSourcePlayer,
            .victimPlayer = scoreVictimPlayer,
            .victimTemplateName = scoreVictimTemplateName,
            .scoreKind = healthScoreKind,
            .scoreTheKillPath = scoreTheKillPath,
            .sourceObjectPresent = scoreSourceEntity.has_value(),
            .victimPlayableSide = scoreVictimPlayableSide,
            .victimIgnoredInGui = scoreVictimIgnoredInGui,
            .victimUnderConstruction = scoreVictimUnderConstruction,
            .sourceIsEnemy = scoreSourceIsEnemy,
            .victimEvaBuilding = victimEvaBuilding,
            .victimEvaUnit = victimEvaUnit,
            .victimPositionFixed = scoreVictimPosition,
            .victimBoundingCircleRadiusFixed = scoreVictimGeometry
                ? scoreVictimGeometry->boundingCircleRadiusFixed
                : math::q32_32{},
            .victimBoundingSphereRadiusFixed = scoreVictimGeometry
                ? scoreVictimGeometry->boundingSphereRadiusFixed
                : math::q32_32{},
            .sourcePositionFixed = scoreSourcePosition,
            .sourceBoundingSphereRadiusFixed = scoreSourceGeometry
                ? scoreSourceGeometry->boundingSphereRadiusFixed
                : math::q32_32{},
            .sourceAirborne =
                (scoreSourceAirborne && scoreSourceAirborne->isAirborne) ||
                (scoreSourceStatus && scoreSourceStatus->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::AirborneTarget))),
        },
        .resolvedDamage = resolvedDamage,
        .clippedDamage = clippedDamage,
        .previousHealth = health.previousFixed,
        .currentHealth = health.currentFixed,
        .maximumHealth = health.maximumFixed,
        .subdualDamage = health.subdualDamageFixed,
        .veterancy = veterancy
            ? veterancy->level : game::ObjectVeterancyLevel::Regular,
        .sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer,
        .sessionSeed = sessionSeed,
        .hasReactionComponent = reactionComponent != nullptr,
        .hasAiDeathGate = plan && plan->hasAiDeathGate,
    };
}

struct DeathWalkExecutionContext final {
    ObjectSimulation& simulation;
    ecs::registry& registry;
    ObjectLifecycle& lifecycle;
    const ObjectSimulationRules& rules;
    ObjectUpgradeExecutionContext& context;
    container::Vector<ObjectDeathEvent>& deathEvents;
    container::Vector<ObjectCrushDieEvent>& crushDieEvents;
    container::Vector<ObjectInstantDeathEffectEvent>& instantDeathEffectEvents;
    container::Vector<ObjectCreateObjectDieEvent>& createObjectDieEvents;
    container::Vector<ObjectCreateCrateDieEvent>& createCrateDieEvents;
    container::Vector<ObjectSpecialPowerCompletionEvent>&
        specialPowerCompletionEvents;
    container::Vector<ObjectFxListDieEffectEvent>& fxListDieEffectEvents;
    container::Vector<ObjectSlowDeathPhaseEvent>& slowDeathPhaseEvents;
    uint64_t& nextFxSequence;

    [[nodiscard]] bool executeNext(ObjectDeathWalkState& deathWalk);
};

using ObjectOnDieCapabilityHandler = void (*)(
    DeathWalkExecutionContext&, ObjectDeathWalkState&,
    const game::ObjectOnDieBehaviorEntry&, ecs::entity);

void ignoreObjectOnDieCapability(
    DeathWalkExecutionContext&, ObjectDeathWalkState&,
    const game::ObjectOnDieBehaviorEntry&, ecs::entity) {}

void rejectReactionAsCapability(
    DeathWalkExecutionContext&, ObjectDeathWalkState&,
    const game::ObjectOnDieBehaviorEntry&, ecs::entity) {
    TD_ASSERT(false);
}

void executeBridgeOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity entity) {
    bool matchingOccurrence = false;
    ObjectBridgeComponent* bridge =
        ecs::try_get<ObjectBridgeComponent>(execution.registry, entity);
    bool matchingBridgeOccurrence = false;
    if (bridge && bridge->plan) {
        matchingBridgeOccurrence = std::any_of(
            bridge->plan->bridges.begin(), bridge->plan->bridges.end(),
            [&](const game::ObjectBridgeBehaviorRule& rule) {
                return rule.authoredOrder == behavior.authoredOrder;
            });
        matchingOccurrence = matchingBridgeOccurrence;
    }
    if (const ObjectBridgeTowerComponent* tower =
            ecs::try_get<ObjectBridgeTowerComponent>(execution.registry,
                                                      entity);
        tower && tower->plan) {
        matchingOccurrence = matchingOccurrence || std::any_of(
            tower->plan->towers.begin(), tower->plan->towers.end(),
            [&](const game::ObjectBridgeTowerRule& rule) {
                return rule.authoredOrder == behavior.authoredOrder;
            });
    }
    if (!matchingOccurrence) return;
    container::Vector<ObjectDamageRequest> damage;
    state(execution.simulation).m_bridge.propagateDeath(
        execution.registry, execution.lifecycle, deathWalk.damage.target,
        deathWalk.damage.confirmedTick, damage);
    for (ObjectDamageRequest& child : damage) {
        execution.simulation.queueDamage(std::move(child));
    }
    if (!matchingBridgeOccurrence) return;

    // BridgeBehavior::onDie collapses its terrain support in this exact
    // authored callback. Publish the navigation/occupant transaction now,
    // rather than waiting for the next generic Bridge update to rediscover
    // effectivelyDead. Mark the projection so that later update is
    // idempotent and cannot emit a second collapse occurrence.
    bridge = ecs::try_get<ObjectBridgeComponent>(execution.registry, entity);
    if (bridge) {
        bridge->navigationStatePublished = true;
        bridge->lastNavigationActive = false;
    }
    auto& simulationState = state(execution.simulation);
    const uint64_t collapseOrdinal =
        execution.simulation.reserveGameplaySubmissionOrdinal();
    simulationState.m_bridgeStateEvents.push_back({
        .object = deathWalk.damage.target,
        .active = false,
        .deathOccurrence = true,
        .authoredOrder = behavior.authoredOrder,
        .submissionOrdinal = collapseOrdinal,
        .confirmedTick = deathWalk.damage.confirmedTick,
    });
    // BridgeDieFX/OCL are BridgeBehavior::update continuations. Their public
    // ordinals follow the synchronous tower/occupant collapse transaction,
    // including the authored zero-delay case.
    simulationState.m_bridge.beginDeathOccurrence(
        execution.registry, execution.lifecycle, execution.context.terrain,
        execution.context.content, execution.rules,
        deathWalk.damage.target, behavior.authoredOrder,
        deathWalk.sessionSeed, deathWalk.damage.confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_bridgeDeathEffects,
        simulationState.m_structureEffectEvents,
        simulationState.m_objectCreationListInvocations);
}

void executeAirfieldOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity entity) {
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(execution.registry, entity);
    if (!airfield || !airfield->plan) return;
    container::Vector<ObjectId> parkedAircraft;
    for (size_t index = 0;
         index < airfield->plan->parkingPlaces.size() &&
         index < airfield->parkingPlaces.size(); ++index) {
        if (airfield->plan->parkingPlaces[index].authoredOrder !=
            behavior.authoredOrder) continue;
        for (const ObjectId aircraft : airfield->parkingPlaces[index].spaces) {
            if (aircraft && aircraft != deathWalk.damage.target &&
                std::find(parkedAircraft.begin(), parkedAircraft.end(),
                          aircraft) == parkedAircraft.end())
                parkedAircraft.push_back(aircraft);
        }
    }
    for (size_t index = 0;
         index < airfield->plan->flightDecks.size() &&
         index < airfield->flightDecks.size(); ++index) {
        if (airfield->plan->flightDecks[index].authoredOrder !=
            behavior.authoredOrder) continue;
        for (const ObjectId aircraft : airfield->flightDecks[index].spaces) {
            if (aircraft && aircraft != deathWalk.damage.target &&
                std::find(parkedAircraft.begin(), parkedAircraft.end(),
                          aircraft) == parkedAircraft.end())
                parkedAircraft.push_back(aircraft);
        }
    }
    uint32_t sequence = 1;
    for (const ObjectId aircraft : parkedAircraft) {
        const std::optional<ecs::entity> aircraftEntity =
            execution.lifecycle.entityFromId(aircraft);
        const ObjectHealthComponent* aircraftHealth = aircraftEntity
            ? ecs::try_get<ObjectHealthComponent>(execution.registry,
                                                   *aircraftEntity)
            : nullptr;
        if (!aircraftEntity || !aircraftHealth ||
            aircraftHealth->effectivelyDead) continue;
        bool takeoffOrLanding = false;
        if (const ObjectAirfieldComponent* aircraftAirfield =
                ecs::try_get<ObjectAirfieldComponent>(execution.registry,
                                                       *aircraftEntity)) {
            takeoffOrLanding = std::any_of(
                aircraftAirfield->jetAi.begin(), aircraftAirfield->jetAi.end(),
                [](const ObjectJetAiRuntime& jet) {
                    return jet.state == ObjectAircraftRuntimeState::TakingOff ||
                        jet.state == ObjectAircraftRuntimeState::Landing;
                });
        }
        if (aboveTerrainLayer(
                execution.registry, *aircraftEntity,
                execution.context.terrain) && !takeoffOrLanding) continue;
        execution.simulation.queueDamage({
            .target = aircraft,
            .sourceSequence = sequence,
            .causalGroup = deathWalk.damage.target,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = deathWalk.damage.confirmedTick,
        });
        if (sequence != std::numeric_limits<uint32_t>::max()) ++sequence;
    }
}

void executeProductionOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity) {
    if (!execution.context.players) return;
    static_cast<void>(ObjectProductionSystem{}.onDie(
        execution.registry, execution.lifecycle, *execution.context.players,
        deathWalk.damage.target, behavior.authoredOrder));
}

void executeSpawnOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity) {
    container::Vector<ObjectDamageRequest> damage;
    static_cast<void>(state(execution.simulation).m_spawnSlave.onSpawnerDie(
        execution.registry, execution.lifecycle, deathWalk.damage.target,
        behavior.authoredOrder, deathWalk.damage.confirmedTick, damage));
    for (ObjectDamageRequest& child : damage) {
        execution.simulation.queueDamage(std::move(child));
    }
}

void executeTechBuildingOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    static_cast<void>(simulationState.m_techBuilding.onDie(
        execution.registry, execution.lifecycle, deathWalk.damage.target,
        behavior.authoredOrder, deathWalk.damage.confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_ownershipChangeRequests));
}

void executePropagandaTowerOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    static_cast<void>(simulationState.m_tactical.onPropagandaTowerDie(
        execution.registry, execution.lifecycle, execution.context.players,
        execution.context.content, simulationState.m_rules,
        execution.context.random, deathWalk.damage.target,
        behavior.authoredOrder, deathWalk.damage.confirmedTick));
}

void executeContainmentOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity entity) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(
            execution.registry, entity);
    const ObjectTransportBehaviorRule* transportBehavior = nullptr;
    if (runtime && runtime->plan) {
        const auto selected = std::find_if(
            runtime->plan->behaviorRules.begin(),
            runtime->plan->behaviorRules.end(),
            [&behavior](const ObjectTransportBehaviorRule& rule) {
                return rule.authoredOrder == behavior.authoredOrder;
            });
        if (selected != runtime->plan->behaviorRules.end())
            transportBehavior = &*selected;
    }
    if (transportBehavior && transportBehavior->kind ==
            ObjectTransportBehaviorKind::BunkerBuster) {
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(execution.registry, entity);
        if (!status || !status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::MissileKillingSelf))) {
            return;
        }
        bool upgradeSatisfied = transportBehavior->upgradeRequired.empty();
        if (!upgradeSatisfied && execution.context.players) {
            const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
                execution.registry, entity);
            const PlayerState* player = owner
                ? execution.context.players->get(owner->player) : nullptr;
            upgradeSatisfied = player && upgradeMaskTest(
                player->upgrades.completed,
                transportBehavior->upgradeRequiredId);
        }
        game::DamageType occupantDamageType =
            game::DamageType::UNRESISTABLE;
        game::DeathType occupantDeathType = game::DeathType::NORMAL;
        bool hasOccupantDamage = false;
        if (execution.context.content &&
            !transportBehavior->occupantDamageWeapon.empty()) {
            const game::WeaponContentId weaponId =
                execution.context.content->findWeaponId(
                    transportBehavior->occupantDamageWeapon);
            if (const game::WeaponTemplate* weapon =
                    execution.context.content->findWeapon(weaponId)) {
                occupantDamageType = weapon->damageType;
                occupantDeathType = weapon->deathType;
                hasOccupantDamage = true;
            }
        }
        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(
                execution.registry, entity);
        container::Vector<ObjectDamageRequest> damage;
        static_cast<void>(simulationState.m_containment.requestBehavior(
            execution.registry, execution.lifecycle, simulationState.m_rules,
            {.kind = ObjectTransportBehaviorRequestKind::BunkerBust,
             .object = deathWalk.damage.target,
             .target = projectile
                 ? projectile->intendedTarget : INVALID_OBJECT_ID,
             .authoredOrder = behavior.authoredOrder,
             .bunkerOccupantDamageType = occupantDamageType,
             .bunkerOccupantDeathType = occupantDeathType,
             .hasBunkerOccupantDamage = hasOccupantDamage,
             .requiredUpgradeSatisfied = upgradeSatisfied,
             .confirmedTick = deathWalk.damage.confirmedTick},
            damage, simulationState.m_containmentEvents,
            simulationState.m_transportEvents,
            simulationState.m_nextGameplaySubmissionOrdinal));
        for (ObjectDamageRequest& child : damage)
            execution.simulation.queueDamage(std::move(child));
        return;
    }
    container::Vector<ObjectDamageRequest> damage;
    TD_ASSERT(!deathWalk.containmentDeathFinalize.has_value());
    simulationState.m_containment.onContainerDie(
        execution.registry, execution.lifecycle, deathWalk.damage.target,
        deathWalk.damage.source, behavior.authoredOrder,
        deathWalk.damage.confirmedTick, damage,
        deathWalk.containmentDeathFinalize);
    for (ObjectDamageRequest& child : damage) {
        execution.simulation.queueDamage(std::move(child));
    }
}

void executeMinefieldOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    const game::ObjectOnDieBehaviorEntry& behavior, ecs::entity) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    if (execution.context.players && execution.context.content &&
        execution.context.terrain && execution.context.random) {
        static_cast<void>(simulationState.m_minefield.onGenerateMinefieldDie(
            execution.registry, execution.lifecycle,
            *execution.context.players, *execution.context.content,
            *execution.context.terrain, *execution.context.random,
            simulationState.m_rules, deathWalk.damage.target,
            behavior.authoredOrder, deathWalk.damage.confirmedTick,
            simulationState.m_nextGameplaySubmissionOrdinal,
            simulationState.m_mineSpawnCommands,
            simulationState.m_minefieldFxEvents));
    }
    static_cast<void>(simulationState.m_minefield.onMinefieldDie(
        execution.registry, execution.lifecycle, deathWalk.damage.target,
        behavior.authoredOrder, deathWalk.damage.confirmedTick));
}

void executeNeutronMissileOnDie(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState&,
    const game::ObjectOnDieBehaviorEntry&, ecs::entity entity) {
    if (ObjectNeutronMissileProjectileComponent* neutron =
            ecs::try_get<ObjectNeutronMissileProjectileComponent>(
                execution.registry, entity)) {
        neutron->deliveryDecalActive = false;
    }
}

constexpr auto kObjectOnDieCapabilityHandlers =
    std::to_array<ObjectOnDieCapabilityHandler>({
        &ignoreObjectOnDieCapability,    // None
        &rejectReactionAsCapability,     // DeathReaction
        &executeBridgeOnDie,
        &executeAirfieldOnDie,
        &executeProductionOnDie,
        &executeSpawnOnDie,
        &executeTechBuildingOnDie,
        &executePropagandaTowerOnDie,
        &executeContainmentOnDie,
        &executeMinefieldOnDie,
        &executeNeutronMissileOnDie,
        &ignoreObjectOnDieCapability,    // Unknown Mod capability
    });

static_assert(kObjectOnDieCapabilityHandlers.size() ==
    static_cast<size_t>(game::ObjectOnDieHandlerKind::Count));

using ObjectDeathReactionHandler = void (*)(
    DeathWalkExecutionContext&, ObjectDeathWalkState&, ecs::entity,
    uint32_t, const game::ObjectDeathReactionRule&,
    game::ObjectStatusMask);

void recordReactionApplied(
    DeathWalkExecutionContext& execution,
    const ObjectDeathWalkState& deathWalk,
    const game::ObjectDeathReactionRule& rule) {
    execution.deathEvents.push_back({
        .kind = ObjectDeathEventKind::ReactionApplied,
        .object = deathWalk.damage.target,
        .source = deathWalk.damage.source,
        .reaction = rule.kind,
        .authoredOrder = rule.authoredOrder,
        .damageType = deathWalk.damage.damageType,
        .deathType = deathWalk.damage.deathType,
        .confirmedTick = deathWalk.damage.confirmedTick,
    });
}

void executeDestroyDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    static_cast<void>(execution.lifecycle.requestDestroy(
        deathWalk.damage.target, ObjectDestroyReason::Combat,
        deathWalk.damage.confirmedTick));
    recordReactionApplied(execution, deathWalk, rule);
}

void executeKeepObjectDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    recordReactionApplied(execution, deathWalk, rule);
}

void executeFxListDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (!isFxListDieActive(execution.registry, entity, ruleIndex, rule)) return;
    emitFxListDieEffect(
        execution.fxListDieEffectEvents, execution.registry,
        execution.lifecycle, entity, deathWalk.damage.target,
        deathWalk.damage, rule, execution.nextFxSequence);
    recordReactionApplied(execution, deathWalk, rule);
}

void executeUpgradeDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    const ObjectProducedByComponent* producedBy =
        ecs::try_get<ObjectProducedByComponent>(execution.registry, entity);
    if (rule.upgradeDie && producedBy && producedBy->producer) {
        static_cast<void>(state(execution.simulation).m_upgrades.removeObjectUpgrade(
            execution.registry, execution.lifecycle, producedBy->producer,
            rule.upgradeDie->upgradeToRemoveId));
    }
    recordReactionApplied(execution, deathWalk, rule);
}

void executeCrushDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (emitCrushDie(
            execution.registry, execution.lifecycle, entity,
            deathWalk.damage.target, deathWalk.damage, rule,
            deathWalk.sessionSeed, execution.crushDieEvents)) {
        recordReactionApplied(execution, deathWalk, rule);
    }
}

void executeFireWeaponWhenDeadReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    const OwnerComponent* victimOwner =
        ecs::try_get<OwnerComponent>(execution.registry, entity);
    const PlayerState* victimPlayer =
        execution.context.players && victimOwner && victimOwner->player
        ? execution.context.players->get(victimOwner->player)
        : nullptr;
    ObjectSimulationState& simulationState = state(execution.simulation);
    if (execution.context.content && execution.context.random &&
        simulationState.m_fireWeaponBehaviors.tryFireWhenDead(
            execution.registry, execution.lifecycle, entity,
            deathWalk.damage.target, ruleIndex, rule,
            victimPlayer ? victimPlayer->upgrades.completed : UpgradeMask{},
            *execution.context.content, *execution.context.random,
            deathWalk.damage.confirmedTick,
            simulationState.m_nextGameplaySubmissionOrdinal,
            simulationState.m_systemWeaponFireCommands)) {
        recordReactionApplied(execution, deathWalk, rule);
    }
}

void executeLeafletDropDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    if (execution.context.players && simulationState.m_leafletDrop.onDie(
            execution.registry, execution.lifecycle,
            *execution.context.players, deathWalk.damage.target,
            rule.authoredOrder, simulationState.m_rules.logicFramesPerSecond,
            deathWalk.damage.confirmedTick)) {
        recordReactionApplied(execution, deathWalk, rule);
    }
}

void executeEjectPilotDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (!rule.ejectPilotDie) return;
    const bool inAir = significantlyAboveTerrain(
        execution.registry, entity, execution.context.terrain,
        execution.rules);
    const container::String& creationList = inAir
        ? rule.ejectPilotDie->airCreationList
        : rule.ejectPilotDie->groundCreationList;
    if (creationList.empty()) return;
    container::String voiceEject;
    container::String soundEject;
    if (const ThingTemplateComponent* objectTemplate =
            ecs::try_get<ThingTemplateComponent>(execution.registry, entity);
        objectTemplate && objectTemplate->archetype) {
        const game::ThingTemplate& templateData =
            objectTemplate->archetype->templateData;
        voiceEject = templateData.voiceEject;
        soundEject = templateData.soundEject;
        // EjectPilotDie resolves both cues through getPerUnitSound(), and all
        // stock content authors them inside the UnitSpecificSounds block
        // rather than as top-level Object fields. Keep the top-level parse for
        // mod content, but fall back to the authored semantic table so shipped
        // pilots are not silent.
        if (voiceEject.empty() || soundEject.empty()) {
            for (const auto& [semanticName, eventName] :
                 templateData.unitSpecificSounds) {
                if (voiceEject.empty() && container::asciiEqualIgnoreCase(
                        semanticName, "VoiceEject")) {
                    voiceEject = eventName;
                } else if (soundEject.empty() &&
                           container::asciiEqualIgnoreCase(
                               semanticName, "SoundEject")) {
                    soundEject = eventName;
                }
            }
        }
    }
    execution.createObjectDieEvents.push_back({
        .kind = ObjectDeathOclEventKind::EjectPilot,
        .object = deathWalk.damage.target,
        .damageSource = deathWalk.damage.source,
        .objectCreationList = creationList,
        .sourcePathfindLayer = deathWalk.sourcePathfindLayer,
        .authoredOrder = rule.authoredOrder,
        .emissionSequence = nextFxEmissionSequence(execution.nextFxSequence),
        .confirmedTick = deathWalk.damage.confirmedTick,
        .voiceEject = std::move(voiceEject),
        .soundEject = std::move(soundEject),
    });
    recordReactionApplied(execution, deathWalk, rule);
}

void executeDamDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    struct Waveguide final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Waveguide> waveguides;
    const auto candidates = ecs::view<
        const ObjectIdentityComponent, const ObjectKindOfComponent>(
            execution.registry);
    waveguides.reserve(candidates.size_hint());
    for (const ecs::entity candidate : candidates) {
        const ObjectIdentityComponent& identity =
            candidates.template get<const ObjectIdentityComponent>(candidate);
        const ObjectKindOfComponent& kinds =
            candidates.template get<const ObjectKindOfComponent>(candidate);
        if (identity.id && hasKind(&kinds, game::ObjectKindOf::Waveguide)) {
            waveguides.push_back({identity.id, candidate});
        }
    }
    std::sort(
        waveguides.begin(), waveguides.end(),
        [](const Waveguide& left, const Waveguide& right) {
            return left.object < right.object;
        });
    for (const Waveguide& waveguide : waveguides) {
        static_cast<void>(ObjectDisabledSystem::clear(
            execution.registry, waveguide.entity,
            ObjectDisabledReason::Default,
            deathWalk.damage.confirmedTick));
    }
    recordReactionApplied(execution, deathWalk, rule);
}

void executeCreateCrateDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (!rule.createCrateDie || !execution.context.content ||
        !execution.context.players) return;
    const OwnerComponent* victimOwner =
        ecs::try_get<OwnerComponent>(execution.registry, entity);
    if (!victimOwner || !victimOwner->player) return;
    const std::optional<ecs::entity> killerEntity =
        execution.lifecycle.entityFromIdIncludingPending(
            deathWalk.damage.source);
    const OwnerComponent* killerOwner = killerEntity
        ? ecs::try_get<OwnerComponent>(execution.registry, *killerEntity)
        : nullptr;
    if (killerOwner && killerOwner->player &&
        relationshipBetweenObjects(
            execution.registry, *execution.context.players, *killerEntity,
            entity) == PlayerRelationship::Allies) return;
    const ObjectKindOfComponent* killerKinds = killerEntity
        ? ecs::try_get<ObjectKindOfComponent>(execution.registry,
                                              *killerEntity)
        : nullptr;
    const ObjectVeterancyComponent* victimVeterancy =
        ecs::try_get<ObjectVeterancyComponent>(execution.registry, entity);
    const game::ObjectVeterancyLevel victimLevel = victimVeterancy
        ? victimVeterancy->level : game::ObjectVeterancyLevel::Regular;
    const uint64_t randomKey = makeDeathRandomKey(
        deathWalk.sessionSeed, deathWalk.damage.target, deathWalk.damage,
        rule.authoredOrder);
    constexpr uint64_t kCrateChancePurpose = 0x435241544543484eull;
    constexpr uint64_t kCrateChoicePurpose = 0x4352415445504943ull;
    constexpr uint64_t kCrateNearAnglePurpose = 0x43524154454e4541ull;
    constexpr uint64_t kCrateWideAnglePurpose = 0x4352415445574944ull;
    constexpr uint64_t kCrateYawPurpose = 0x4352415445594157ull;
    bool createdAny = false;
    uint64_t crateDataIndex = 0;
    for (const container::String& crateData :
         rule.createCrateDie->crateData) {
        const game::CrateTemplateDefinition* definition =
            execution.context.content->findCrateTemplate(crateData);
        const uint64_t indexedPurpose = mixDeathRandom(crateDataIndex++);
        if (!definition) continue;
        if (!(deathRandomUnit(
                  randomKey, kCrateChancePurpose ^ indexedPurpose) <
              definition->creationChance)) continue;
        if (definition->veterancyLevel &&
            *definition->veterancyLevel != victimLevel) continue;
        if (definition->killedByKindMask.any() &&
            (!killerKinds || !killerKinds->mask.test_for_all(
                definition->killedByKindMask))) continue;
        if (!definition->killerScience.empty() &&
            (!killerOwner || !killerOwner->player ||
             !execution.context.players->hasScience(
                 killerOwner->player, definition->killerScience))) continue;

        const math::q32_32 pick = deathRandomUnit(
            randomKey, kCrateChoicePurpose ^ indexedPurpose);
        math::q32_32 running;
        const game::CrateObjectChoice* selected = nullptr;
        for (const game::CrateObjectChoice& choice :
             definition->possibleCrates) {
            running += choice.chance;
            if (running > pick) {
                selected = &choice;
                break;
            }
        }
        if (!selected || selected->objectTemplate.empty() ||
            !execution.context.content->findObjectArchetype(
                selected->objectTemplate)) continue;
        const TransformComponent* victimTransform =
            ecs::try_get<TransformComponent>(execution.registry, entity);
        const ObjectAirborneComponent* victimAirborne =
            ecs::try_get<ObjectAirborneComponent>(execution.registry, entity);
        const ObjectTerrainLayerComponent* victimLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(execution.registry,
                                                       entity);
        const math::q32_32 fullTurn =
            math::q32_32::from_raw(26'986'075'409ll);
        execution.createCrateDieEvents.push_back({
            .object = deathWalk.damage.target,
            .damageSource = deathWalk.damage.source,
            .crateObjectTemplate = selected->objectTemplate,
            .makerOwner = victimOwner->player,
            .owner = definition->ownedByMaker
                ? victimOwner->player : NEUTRAL_PLAYER_ID,
            .sourcePosition = victimTransform
                ? readAuthoritativeObjectPosition(
                    execution.registry, entity, *victimTransform)
                : LogicFixedVec3{},
            .sourcePathfindLayer = victimLayer
                ? victimLayer->pathfindLayer
                : game::terrain::kGroundPathfindLayer,
            .sourceAirborne = victimAirborne && victimAirborne->isAirborne,
            .nearSearchAngleRadians = deathRandomUnit(
                randomKey, kCrateNearAnglePurpose ^ indexedPurpose) *
                fullTurn,
            .wideSearchAngleRadians = deathRandomUnit(
                randomKey, kCrateWideAnglePurpose ^ indexedPurpose) *
                fullTurn,
            .orientationRadians = deathRandomUnit(
                randomKey, kCrateYawPurpose ^ indexedPurpose) * fullTurn,
            .authoredOrder = rule.authoredOrder,
            .emissionSequence = nextFxEmissionSequence(
                execution.nextFxSequence),
            .confirmedTick = deathWalk.damage.confirmedTick,
        });
        createdAny = true;
    }
    if (createdAny) recordReactionApplied(execution, deathWalk, rule);
}

void executeCreateObjectDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    if (!rule.createObjectDie ||
        rule.createObjectDie->creationList.empty()) return;
    execution.createObjectDieEvents.push_back({
        .object = deathWalk.damage.target,
        .damageSource = deathWalk.damage.source,
        .objectCreationList = rule.createObjectDie->creationList,
        .previousHealth = deathWalk.previousHealth,
        .maximumHealth = deathWalk.maximumHealth,
        .subdualDamage = deathWalk.subdualDamage,
        .sourcePathfindLayer = deathWalk.sourcePathfindLayer,
        .authoredOrder = rule.authoredOrder,
        .emissionSequence = nextFxEmissionSequence(execution.nextFxSequence),
        .confirmedTick = deathWalk.damage.confirmedTick,
        .transferPreviousHealth =
            rule.createObjectDie->transferPreviousHealth,
        .transferSelection = rule.createObjectDie->transferSelection,
    });
    recordReactionApplied(execution, deathWalk, rule);
}

void executeSpecialPowerCompletionDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    static_cast<void>(emitSpecialPowerCompletion(
        execution.registry, entity, deathWalk.damage.target, ruleIndex, rule,
        deathWalk.damage.confirmedTick,
        state(execution.simulation).m_nextGameplaySubmissionOrdinal,
        execution.specialPowerCompletionEvents));
    recordReactionApplied(execution, deathWalk, rule);
}

void executeNeutronBlastDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (!rule.neutronBlastDie) return;
    ObjectSimulationState& simulationState = state(execution.simulation);
    container::Vector<ObjectDamageRequest> blastDamage;
    executeNeutronBlastDeath(
        execution.registry, execution.lifecycle, execution.context.players,
        entity, deathWalk.damage.target, *rule.neutronBlastDie,
        rule.authoredOrder, deathWalk.damage.confirmedTick, blastDamage,
        simulationState.m_vehicleNeutralizationRequests);
    for (ObjectDamageRequest& child : blastDamage) {
        execution.simulation.queueDamage(std::move(child));
    }
    recordReactionApplied(execution, deathWalk, rule);
}

void executeStructureDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    const bool applied = simulationState.m_structureDestruction.begin(
        execution.registry, execution.lifecycle, entity,
        deathWalk.damage.target, deathWalk.damage.source,
        deathWalk.damage.damageType, deathWalk.plan, ruleIndex,
        simulationState.m_rules, simulationState.m_sessionSeed,
        deathWalk.damage.confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_structureEffectEvents);
    if (!applied) return;
    if (deathWalk.hasAiDeathGate) deathWalk.aiDeathClaimed = true;
    recordReactionApplied(execution, deathWalk, rule);
}

[[nodiscard]] bool isJetGroundDeathOccurrence(
    DeathWalkExecutionContext& execution, ecs::entity entity,
    uint32_t authoredOrder) {
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(execution.registry, entity);
    if (!airfield || !airfield->plan) return false;
    const auto found = std::find_if(
        airfield->plan->slowDeaths.begin(),
        airfield->plan->slowDeaths.end(),
        [authoredOrder](const game::ObjectAircraftSlowDeathRule& rule) {
            return rule.authoredOrder == authoredOrder &&
                rule.kind == game::ObjectAircraftSlowDeathKind::Jet;
        });
    if (found == airfield->plan->slowDeaths.end()) return false;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(execution.registry, entity);
    const bool onDeck = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::DeckHeightOffset));
    return onDeck || !significantlyAboveTerrain(
        execution.registry, entity, execution.context.terrain,
        execution.rules);
}

void executePooledSlowDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, const game::ObjectDeathReactionRule& triggerRule,
    game::ObjectStatusMask statuses) {
    if (deathWalk.hasAiDeathGate && deathWalk.aiDeathClaimed) return;

    struct SlowCandidate final {
        uint32_t ruleIndex = 0;
        uint64_t weight = 1;
    };
    container::Vector<SlowCandidate> candidates;
    uint64_t totalWeight = 0;
    for (uint32_t candidateIndex = 0;
         candidateIndex < deathWalk.plan->rules.size(); ++candidateIndex) {
        const game::ObjectDeathReactionRule& candidateRule =
            deathWalk.plan->rules[candidateIndex];
        const bool slowDeathInterface =
            candidateRule.kind == game::ObjectDeathReactionKind::SlowDeath ||
            candidateRule.kind ==
                game::ObjectDeathReactionKind::AircraftSlowDeath;
        if (!slowDeathInterface || !candidateRule.slowDeath ||
            !game::isObjectDeathReactionApplicable(
                candidateRule, deathWalk.damage.deathType,
                deathWalk.veterancy, statuses)) {
            continue;
        }
        const uint64_t weight = slowDeathWeight(
            *candidateRule.slowDeath, deathWalk.resolvedDamage,
            deathWalk.clippedDamage, deathWalk.maximumHealth);
        candidates.push_back({candidateIndex, weight});
        totalWeight = saturatingAdd(totalWeight, weight);
    }
    if (candidates.empty() || totalWeight == 0) return;
    const uint64_t randomKey = makeDeathRandomKey(
        deathWalk.sessionSeed, deathWalk.damage.target, deathWalk.damage,
        triggerRule.authoredOrder);
    uint64_t roll = randomInclusive(randomKey, 0u, 1u, totalWeight);
    uint32_t selectedRuleIndex = candidates.back().ruleIndex;
    for (const SlowCandidate& candidate : candidates) {
        if (roll <= candidate.weight) {
            selectedRuleIndex = candidate.ruleIndex;
            break;
        }
        roll -= candidate.weight;
    }
    const game::ObjectDeathReactionRule& selected =
        deathWalk.plan->rules[selectedRuleIndex];
    bool applied = false;
    if (selected.kind ==
        game::ObjectDeathReactionKind::AircraftSlowDeath) {
        ObjectSimulationState& simulationState = state(execution.simulation);
        // Selection through SlowDeathBehaviorInterface::beginSlowDeath skips
        // JetSlowDeathBehavior::onDie's ground-only branch. The branch is
        // evaluated only for the authored Jet callback that triggered this
        // pool, immediately before entering here.
        applied = simulationState.m_airfield.beginAircraftSlowDeathOnDie(
            execution.registry, execution.lifecycle,
            simulationState.m_rules, execution.context.terrain,
            deathWalk.damage.target, selected.authoredOrder,
            deathWalk.damage.confirmedTick,
            simulationState.m_airfieldEvents,
            simulationState.m_slowDeathPhaseEvents,
            simulationState.m_deleteDestroyRequests,
            simulationState.m_nextGameplaySubmissionOrdinal, true);
        if (applied) recordReactionApplied(execution, deathWalk, selected);
    } else {
        scheduleSlowDeath(
            execution.registry, entity, deathWalk.damage.target,
            deathWalk.damage, *deathWalk.plan, selectedRuleIndex,
            *selected.slowDeath, deathWalk.resolvedDamage,
            deathWalk.clippedDamage, deathWalk.maximumHealth,
            execution.rules, execution.context.terrain,
            deathWalk.sessionSeed, execution.deathEvents,
            execution.slowDeathPhaseEvents, execution.nextFxSequence);
        applied = true;
    }
    if (applied && deathWalk.hasAiDeathGate) {
        deathWalk.aiDeathClaimed = true;
    }
}

void executeAircraftSlowDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask statuses) {
    if (!rule.slowDeath) return;
    if (isJetGroundDeathOccurrence(
            execution, entity, rule.authoredOrder)) {
        ObjectSimulationState& simulationState = state(execution.simulation);
        const bool applied =
            simulationState.m_airfield.beginAircraftSlowDeathOnDie(
                execution.registry, execution.lifecycle,
                simulationState.m_rules, execution.context.terrain,
                deathWalk.damage.target, rule.authoredOrder,
                deathWalk.damage.confirmedTick,
                simulationState.m_airfieldEvents,
                simulationState.m_slowDeathPhaseEvents,
                simulationState.m_deleteDestroyRequests,
                simulationState.m_nextGameplaySubmissionOrdinal);
        if (applied) recordReactionApplied(execution, deathWalk, rule);
        // JetSlowDeathBehavior::onDie does not call SlowDeathBehavior::onDie
        // on the ground and therefore does not mark the shared AI death gate.
        return;
    }
    executePooledSlowDeathReaction(
        execution, deathWalk, entity, rule, statuses);
}

void executeRebuildHoleDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    ObjectSimulationState& simulationState = state(execution.simulation);
    if (rule.kind == game::ObjectDeathReactionKind::RebuildHoleExpose) {
        simulationState.m_rebuildHole.onExposeDie(
            execution.registry, execution.lifecycle, deathWalk.damage.target,
            deathWalk.damage.source, rule.authoredOrder,
            deathWalk.damage.confirmedTick,
            simulationState.m_nextGameplaySubmissionOrdinal,
            simulationState.m_rebuildExposeIntents);
    } else {
        simulationState.m_rebuildHole.onBehaviorDie(
            execution.registry, execution.lifecycle, deathWalk.damage.target,
            rule.authoredOrder, deathWalk.damage.confirmedTick);
    }
    recordReactionApplied(execution, deathWalk, rule);
}

void executeInstantDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule, game::ObjectStatusMask) {
    if (deathWalk.hasAiDeathGate && deathWalk.aiDeathClaimed) return;
    if (deathWalk.hasAiDeathGate) deathWalk.aiDeathClaimed = true;
    emitInstantDeathEffect(
        execution.instantDeathEffectEvents, execution.registry, entity,
        deathWalk.damage.target, deathWalk.damage, rule,
        deathWalk.sessionSeed, execution.nextFxSequence);
    static_cast<void>(execution.lifecycle.requestDestroy(
        deathWalk.damage.target, ObjectDestroyReason::Combat,
        deathWalk.damage.confirmedTick));
    recordReactionApplied(execution, deathWalk, rule);
}

void executeSlowDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity entity, uint32_t,
    const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask statuses) {
    if (!rule.slowDeath) return;
    if (rule.slowDeath->cancelsBattleBusUndeath) {
        static_cast<void>(state(execution.simulation).m_containment
            .cancelBattleBusUndeathForRealDeath(
                execution.registry, execution.lifecycle,
                deathWalk.damage.target, rule.authoredOrder));
    }
    executePooledSlowDeathReaction(
        execution, deathWalk, entity, rule, statuses);
}

void executeUnsupportedDeathReaction(
    DeathWalkExecutionContext& execution, ObjectDeathWalkState& deathWalk,
    ecs::entity, uint32_t, const game::ObjectDeathReactionRule& rule,
    game::ObjectStatusMask) {
    execution.deathEvents.push_back({
        .kind = ObjectDeathEventKind::UnsupportedReaction,
        .object = deathWalk.damage.target,
        .source = deathWalk.damage.source,
        .reaction = rule.kind,
        .authoredOrder = rule.authoredOrder,
        .damageType = deathWalk.damage.damageType,
        .deathType = deathWalk.damage.deathType,
        .confirmedTick = deathWalk.damage.confirmedTick,
    });
}

// This table is the executable capability catalog for authored Die modules.
// Its source order mirrors ObjectDeathReactionKind; adding a new typed kind
// without an execution semantic is therefore a compile-time error.
constexpr auto kObjectDeathReactionHandlers =
    std::to_array<ObjectDeathReactionHandler>({
        &executeDestroyDeathReaction,
        &executeKeepObjectDeathReaction,
        &executeFxListDeathReaction,
        &executeUpgradeDeathReaction,
        &executeCrushDeathReaction,
        &executeFireWeaponWhenDeadReaction,
        &executeLeafletDropDeathReaction,
        &executeEjectPilotDeathReaction,
        &executeDamDeathReaction,
        &executeCreateCrateDeathReaction,
        &executeCreateObjectDeathReaction,
        &executeSpecialPowerCompletionDeathReaction,
        &executeNeutronBlastDeathReaction,
        &executeStructureDeathReaction,
        &executeStructureDeathReaction,
        &executeAircraftSlowDeathReaction,
        &executeRebuildHoleDeathReaction,
        &executeRebuildHoleDeathReaction,
        &executeInstantDeathReaction,
        &executeSlowDeathReaction,
        &executeUnsupportedDeathReaction,
    });

static_assert(kObjectDeathReactionHandlers.size() ==
    static_cast<size_t>(game::ObjectDeathReactionKind::Count));

bool DeathWalkExecutionContext::executeNext(ObjectDeathWalkState &deathWalk) {
    const ObjectDamageRequest &request = deathWalk.damage;
    const container::SharedPtr<const game::ObjectDeathReactionPlan> &reactionPlan = deathWalk.plan;
    TD_ASSERT(reactionPlan != nullptr);
    if (!reactionPlan)
        return false;

    // Resume the exact authored Contain occurrence only after all passenger
    // Damage children emitted by it have closed. This is the explicit form of
    // OpenContain::onDie's synchronous "damage, then removeAllContained"
    // ordering and keeps the next Behavior from observing stale occupants.
    if (deathWalk.containmentDeathFinalize) {
        ObjectSimulationState& simulationState = state(simulation);
        container::Vector<ObjectDamageRequest> damage;
        const ObjectContainmentDeathFinalizeAdvance advance =
            simulationState.m_containment.advanceContainerDie(
                registry, lifecycle, *deathWalk.containmentDeathFinalize,
                context.terrain, context.navigation,
                simulationState.m_nextGameplaySubmissionOrdinal, damage,
                simulationState.m_deleteDestroyRequests);
        for (ObjectDamageRequest& child : damage) {
            simulation.queueDamage(std::move(child));
        }
        if (advance ==
            ObjectContainmentDeathFinalizeAdvance::ChildrenEmitted) {
            return true;
        }
        deathWalk.containmentDeathFinalize.reset();
    }

    const std::optional<game::ObjectOnDieBehaviorEntry> behavior =
        takeNextObjectDeathBehavior(deathWalk);
    if (!behavior)
        return false;
    if (behavior->handler != game::ObjectOnDieHandlerKind::DeathReaction) {
        const std::optional<ecs::entity> currentEntity =
            lifecycle.entityFromIdIncludingPending(deathWalk.damage.target);
        if (!currentEntity) {
            deathWalk.nextBehaviorIndex = static_cast<uint32_t>(
                reactionPlan->onDieBehaviors.size());
            return false;
        }
        const ecs::entity entity = *currentEntity;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const game::ObjectStatusMask statuses = status ? status->flags : 0;
        if (!game::isObjectOnDieBehaviorApplicable(
                *behavior, request.deathType, deathWalk.veterancy,
                statuses)) {
            return true;
        }
        const size_t handlerIndex = static_cast<size_t>(behavior->handler);
        TD_ASSERT(handlerIndex < kObjectOnDieCapabilityHandlers.size());
        if (handlerIndex < kObjectOnDieCapabilityHandlers.size()) {
            kObjectOnDieCapabilityHandlers[handlerIndex](
                *this, deathWalk, *behavior, entity);
        }
        return true;
    }
    if (behavior->reactionRuleIndex >= reactionPlan->rules.size()) {
        return true;
    }
    const uint32_t ruleIndex = behavior->reactionRuleIndex;
    const game::ObjectDeathReactionRule &rule = reactionPlan->rules[ruleIndex];
    const std::optional<ecs::entity> currentEntity =
        lifecycle.entityFromIdIncludingPending(deathWalk.damage.target);
    TD_ASSERT(currentEntity.has_value());
    if (!currentEntity) {
        deathWalk.nextBehaviorIndex = static_cast<uint32_t>(reactionPlan->onDieBehaviors.size());
        return false;
    }
    // Never retain a component reference across a handler barrier.
    // Deferred destruction keeps the entity resolvable; an actual
    // removal terminates the remaining authored walk deterministically.
    const ecs::entity entity = *currentEntity;
    const ObjectStatusComponent *status = ecs::try_get<ObjectStatusComponent>(registry, entity);
    const game::ObjectStatusMask statuses = status ? status->flags : 0;
    if (!game::isObjectDeathReactionApplicable(rule, request.deathType, deathWalk.veterancy,
                                               statuses)) {
        return true;
    }

    const size_t handlerIndex = static_cast<size_t>(rule.kind);
    TD_ASSERT(handlerIndex < kObjectDeathReactionHandlers.size());
    if (handlerIndex < kObjectDeathReactionHandlers.size()) {
        kObjectDeathReactionHandlers[handlerIndex](
            *this, deathWalk, entity, ruleIndex, rule, statuses);
    }
    return true;
}

} // namespace

void requestDeath(ObjectSimulation& simulation, ecs::registry& registry,
                  ObjectLifecycle& lifecycle, ecs::entity entity,
                  ObjectHealthComponent& health,
                  const ObjectDamageRequest& incomingRequest,
                  HealthScalar resolvedDamage, HealthScalar clippedDamage,
                  ObjectUpgradeExecutionContext context,
                  uint64_t sessionSeed, bool scoreTheKillPath,
                  ObjectDamageTransactionResult* transactionResult) {
    if (health.terminalDeathIssued) return;
    ObjectDeathWalkState deathWalk = beginDeathWalk(
        registry, lifecycle, entity, health, incomingRequest,
        resolvedDamage, clippedDamage, context, sessionSeed,
        scoreTheKillPath);
    if (context.content) {
        ObjectSimulationState& simulationState = state(simulation);
        container::Vector<ObjectDamageRequest> boobyTrapDamage;
        static_cast<void>(
            simulationState.m_stickyBomb.detonateBoobyTrapsOnDyingTarget(
                registry, lifecycle, *context.content,
                incomingRequest.target, incomingRequest.confirmedTick,
                boobyTrapDamage,
                simulationState.m_stickyBombPresentationEvents));
        for (ObjectDamageRequest& child : boobyTrapDamage) {
            simulation.queueDamage(std::move(child));
        }
    }
    if (transactionResult) {
        TD_ASSERT(!transactionResult->deathWalk.has_value());
        transactionResult->deathWalk = std::move(deathWalk);
        return;
    }
    for (;;) {
        const ObjectDeathWalkAdvance advance = simulation.advanceDeathWalk(
            registry, lifecycle, deathWalk, context);
        if (advance == ObjectDeathWalkAdvance::BehaviorHandled) {
            continue;
        }
        if (advance == ObjectDeathWalkAdvance::InvalidState) {
            TD_ASSERT(false);
            return;
        }
        break;
    }
    static_cast<void>(simulation.completeDeathWalk(std::move(deathWalk)));
}

} // namespace engine::object_simulation_detail

namespace engine {

ObjectDeathWalkAdvance ObjectSimulation::advanceDeathWalk(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectDeathWalkState& deathWalk, ObjectUpgradeExecutionContext context) {
    auto& simulationState = object_simulation_detail::state(*this);
    const auto enterPostamble = [&] {
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(deathWalk.damage.target);
        if (entity) {
            math::q32_32 terrainDecalFadeRate =
                math::q32_32::from_fraction(3, 100);
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *entity);
            const ObjectSlowDeathRuntimeComponent* slowRuntime =
                ecs::try_get<ObjectSlowDeathRuntimeComponent>(registry,
                                                               *entity);
            const ObjectDeathReactionComponent* reactions =
                ecs::try_get<ObjectDeathReactionComponent>(registry, *entity);
            if (kinds && game::objectHasKind(
                    kinds->mask, game::ObjectKindOf::Infantry) && slowRuntime &&
                reactions && reactions->plan &&
                slowRuntime->selectedRuleIndex <
                    reactions->plan->rules.size()) {
                const game::ObjectDeathReactionRule& selected =
                    reactions->plan->rules[slowRuntime->selectedRuleIndex];
                if (selected.slowDeath &&
                    selected.slowDeath->sinkRateUnitsPerSecond !=
                        math::q32_32{}) {
                    terrainDecalFadeRate =
                        math::q32_32::from_fraction(1, 5);
                }
            }
            setObjectTerrainDecalFade(
                registry, *entity, {}, terrainDecalFadeRate,
                deathWalk.damage.confirmedTick);
        }
        simulationState.m_spawnSlave.onSpawnedObjectDie(
            registry, lifecycle, simulationState.m_rules,
            deathWalk.damage.target, deathWalk.damage.confirmedTick);
        // RefCode performs reconstructing-building target transfer in the
        // fixed Object::onDie suffix, after every authored Die callback. Keep
        // that object-level ordering, but emit a typed capability transaction
        // instead of rescanning accumulated diagnostic death events in a
        // later simulation phase.
        simulationState.m_rebuildHole.onDeathPostamble(
            registry, lifecycle, deathWalk.damage.target,
            simulationState.m_rules, deathWalk.damage.confirmedTick,
            simulationState.m_nextGameplaySubmissionOrdinal,
            simulationState.m_rebuildTargetRemapIntents);
        simulationState.m_deathEvents.push_back({
            .kind = ObjectDeathEventKind::Postamble,
            .object = deathWalk.damage.target,
            .source = deathWalk.damage.source,
            .damageType = deathWalk.damage.damageType,
            .deathType = deathWalk.damage.deathType,
            .confirmedTick = deathWalk.damage.confirmedTick,
        });
        deathWalk.phase = ObjectDeathWalkPhase::Postamble;
    };
    if (deathWalk.phase == ObjectDeathWalkPhase::Preamble) {
        if (!deathWalk.hasReactionComponent) {
            // Programmatic helper objects may have no frozen archetype. Final
            // parsed content remains strictly driven by its authored Die list.
            static_cast<void>(lifecycle.requestDestroy(
                deathWalk.damage.target, ObjectDestroyReason::Combat,
                deathWalk.damage.confirmedTick));
            simulationState.m_deathEvents.push_back({
                .kind = ObjectDeathEventKind::UnprofiledFallbackDestroy,
                .object = deathWalk.damage.target,
                .source = deathWalk.damage.source,
                .damageType = deathWalk.damage.damageType,
                .deathType = deathWalk.damage.deathType,
                .confirmedTick = deathWalk.damage.confirmedTick,
            });
            enterPostamble();
            return ObjectDeathWalkAdvance::ReadyForPostamble;
        }
        deathWalk.phase = deathWalk.plan
            ? ObjectDeathWalkPhase::Behaviors
            : ObjectDeathWalkPhase::Postamble;
    }
    if (deathWalk.phase == ObjectDeathWalkPhase::Postamble) {
        return ObjectDeathWalkAdvance::ReadyForPostamble;
    }
    if (deathWalk.phase != ObjectDeathWalkPhase::Behaviors) {
        return ObjectDeathWalkAdvance::InvalidState;
    }

    object_simulation_detail::DeathWalkExecutionContext executionContext{
        .simulation = *this,
        .registry = registry,
        .lifecycle = lifecycle,
        .rules = simulationState.m_rules,
        .context = context,
        .deathEvents = simulationState.m_deathEvents,
        .crushDieEvents = simulationState.m_crushDieEvents,
        .instantDeathEffectEvents = simulationState.m_instantDeathEffectEvents,
        .createObjectDieEvents = simulationState.m_createObjectDieEvents,
        .createCrateDieEvents = simulationState.m_createCrateDieEvents,
        .specialPowerCompletionEvents =
            simulationState.m_specialPowerCompletionEvents,
        .fxListDieEffectEvents = simulationState.m_fxListDieEffectEvents,
        .slowDeathPhaseEvents = simulationState.m_slowDeathPhaseEvents,
        .nextFxSequence = simulationState.m_nextGameplaySubmissionOrdinal,
    };
    if (executionContext.executeNext(deathWalk)) {
        return ObjectDeathWalkAdvance::BehaviorHandled;
    }
    enterPostamble();
    return ObjectDeathWalkAdvance::ReadyForPostamble;
}

bool ObjectSimulation::completeDeathWalk(ObjectDeathWalkState deathWalk) {
    if (deathWalk.phase != ObjectDeathWalkPhase::Postamble) return false;
    deathWalk.phase = ObjectDeathWalkPhase::Completed;
    object_simulation_detail::state(*this).m_healthEvents.push_back(
        std::move(deathWalk.diedEvent));
    return true;
}

} // namespace engine

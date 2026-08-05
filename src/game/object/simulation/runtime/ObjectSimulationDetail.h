#pragma once

#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/definition/ModelConditionState.h"

namespace game {
struct ObjectPhysicsPlan;
}

namespace engine::object_simulation_detail {

using HealthScalar = ObjectHealthComponent::Scalar;

struct ObjectPhysicsScratch;

[[nodiscard]] container::Vector<game::FrozenLocomotorTemplate>
collectRuntimeLocomotors(const game::ThingTemplate& objectTemplate,
                         const GameContentSnapshot& content,
                         game::LocomotorSetSlot slot);
void applyLocomotorTemplate(ObjectLocomotionComponent& runtime,
                            const game::FrozenLocomotorTemplate& locomotor);
void chooseLocomotorForPosition(ObjectLocomotionComponent& locomotion,
                                const game::terrain::TerrainLogic& terrain,
                                math::q32_32 x, math::q32_32 y);
void releaseAIMovementPath(
    navigation::NavigationSystem* navigation,
    const ObjectAIPathMovementComponent& movement) noexcept;
// Terrain-conforming chassis attitude for a grounded ground locomotor. It is
// defined beside the other locomotor helpers but driven from the Physics pass,
// because Movement deliberately sleeps settled objects and a parked vehicle on
// a hillside still has to stay conformed.
// Returns true when the published pitch/roll pair actually changed, so the
// caller can mark render extraction dirty without waking every object.
[[nodiscard]] bool updateLocomotorAttitude(
    const ObjectLocomotionComponent& locomotion,
    ObjectPhysicsComponent& physics,
    const ObjectFixedTransformComponent& fixedTransform,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, bool airborne,
    bool immobile) noexcept;
void updateMovement(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, uint64_t tick,
    const ObjectSimulationRules& rules,
    container::Vector<ObjectMovementEvent>& events,
    container::Vector<ai::MovementFeedback>& aiFeedback,
    container::Span<const ObjectId> aiMoveStopOwners,
    container::Span<const ObjectId> aiAttackOwners,
    container::Span<const ObjectAIMovementCommand> aiMovementCommands,
    navigation::NavigationSystem* navigation);
void updateAIFacing(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Span<const ai::AIStateCommand> sourceCommands,
    container::Vector<ai::AIFacingFeedback>& feedback);

[[nodiscard]] ObjectPhysicsComponent compilePhysicsComponent(
    const game::ObjectPhysicsPlan& plan,
    const ObjectFixedTransformComponent& transform,
    const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain, uint32_t pathfindLayer,
    const ObjectSimulationRules& rules);
void rebuildPhysicsOrientation(ObjectPhysicsComponent& physics) noexcept;
void setPhysicsModelCondition(ecs::registry& registry, ecs::entity entity,
                              game::ModelConditionFlag condition, bool enabled);
[[nodiscard]] math::q32_32 physicsLayerHeight(
    const game::terrain::TerrainLogic& terrain,
    const ecs::registry& registry, ecs::entity entity,
    const LogicFixedVec3& position) noexcept;
void updatePhysics(ecs::registry& registry, ObjectLifecycle& lifecycle,
                   const game::terrain::TerrainLogic& terrain,
                   const ObjectSimulationRules& rules, uint64_t tick,
                   container::Vector<ObjectPhysicsEvent>& events,
                   container::Vector<ObjectDamageRequest>& outDamage,
                   ObjectPhysicsScratch& scratch);
void resolvePhysicsCollisions(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry* players, navigation::NavigationSystem* navigation,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectPilotVehicleTakeoverRequest>& takeoverRequests,
    container::Vector<ObjectPhysicsCrashCommand>& crashCommands,
    container::Vector<ObjectAIMovementObstructionEvent>&
        obstructionEvents,
    ObjectPhysicsScratch& scratch,
    ObjectSpatialIndex& broadPhase);

void initializeResolvedArmor(ecs::registry& registry, ecs::entity entity,
                             const GameContentSnapshot& content);
void projectBodyDamageVisual(ObjectBodyDamageState state,
                             RenderModelComponent& visual) noexcept;
void updateSubdualRecovery(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectHealthEvent>& events);
void updateTimedStatusDamage(ecs::registry& registry,
                             uint64_t confirmedTick);

void executeNeutronBlastDeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry* players, ecs::entity sourceEntity, ObjectId source,
    const game::ObjectNeutronBlastDieParameters& parameters,
    uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damage,
    container::Vector<ObjectVehicleNeutralizationRequest>& events);
[[nodiscard]] uint64_t mixDeathRandom(uint64_t value) noexcept;
[[nodiscard]] uint64_t makeDeathRandomKey(
    uint64_t sessionSeed, ObjectId object,
    const ObjectDamageRequest& request, uint32_t authoredOrder) noexcept;
[[nodiscard]] uint64_t nextFxEmissionSequence(uint64_t& next) noexcept;
[[nodiscard]] uint64_t randomInclusive(
    uint64_t key, uint64_t purpose, uint64_t minimum,
    uint64_t maximum) noexcept;
[[nodiscard]] math::q32_32 deathRandomUnit(
    uint64_t key, uint64_t purpose) noexcept;
[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept;
[[nodiscard]] uint64_t slowDeathWeight(
    const game::ObjectSlowDeathParameters& parameters,
    HealthScalar resolvedDamage, HealthScalar clippedDamage,
    HealthScalar maximumHealth) noexcept;
void emitInstantDeathEffect(
    container::Vector<ObjectInstantDeathEffectEvent>& events,
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    const ObjectDamageRequest& request,
    const game::ObjectDeathReactionRule& rule, uint64_t sessionSeed,
    uint64_t& nextFxSequence);
[[nodiscard]] bool isFxListDieActive(
    const ecs::registry& registry, ecs::entity entity, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule) noexcept;
void emitFxListDieEffect(
    container::Vector<ObjectFxListDieEffectEvent>& events,
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, const ObjectDamageRequest& request,
    const game::ObjectDeathReactionRule& rule, uint64_t& nextFxSequence);
void scheduleSlowDeath(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    const ObjectDamageRequest& request,
    const game::ObjectDeathReactionPlan& plan, uint32_t selectedRuleIndex,
    const game::ObjectSlowDeathParameters& parameters,
    HealthScalar resolvedDamage, HealthScalar clippedDamage,
    HealthScalar maximumHealth, const ObjectSimulationRules& rules,
    const game::terrain::TerrainLogic* terrain, uint64_t sessionSeed,
    container::Vector<ObjectDeathEvent>& deathEvents,
    container::Vector<ObjectSlowDeathPhaseEvent>& phaseEvents,
    uint64_t& nextFxSequence);
void updateSlowDeaths(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic* terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectSlowDeathPhaseEvent>& phaseEvents,
    uint64_t& nextFxSequence);
[[nodiscard]] bool emitCrushDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity victimEntity, ObjectId victim,
    const ObjectDamageRequest& request,
    const game::ObjectDeathReactionRule& rule, uint64_t sessionSeed,
    container::Vector<ObjectCrushDieEvent>& events);
[[nodiscard]] bool significantlyAboveTerrain(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain,
    const ObjectSimulationRules& rules) noexcept;
[[nodiscard]] bool aboveTerrainLayer(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain) noexcept;
void detachDeadAircraftReservations(
    ObjectAirfieldSystem& airfieldSystem, ecs::registry& registry,
    const ObjectLifecycle& lifecycle, ObjectId aircraft,
    uint64_t confirmedTick, container::Vector<ObjectAirfieldEvent>& events,
    std::optional<uint32_t> authoredOrder = std::nullopt);
[[nodiscard]] bool emitSpecialPowerCompletion(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    uint32_t ruleIndex, const game::ObjectDeathReactionRule& rule,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectSpecialPowerCompletionEvent>& events);

} // namespace engine::object_simulation_detail

#pragma once

#include "game/object/simulation/economy/ObjectProduction.h"

namespace engine::production_detail {

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool advanceProductionPresentation(
    ObjectProductionComponent& component, uint64_t confirmedTick,
    uint32_t framesPerSecond) noexcept;
[[nodiscard]] bool prepareCompletedUnitExit(
    ObjectProductionComponent& component, ObjectProductionJob& job,
    uint64_t confirmedTick, bool& changed) noexcept;

[[nodiscard]] bool producerCanResearchUpgrade(
    ecs::registry& registry, ecs::entity producer,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player,
    const UpgradeDefinition& upgrade,
    ObjectUpgradeProductionAdmission admission);
[[nodiscard]] bool producerCanBuildUnit(
    ecs::registry& registry, ecs::entity producer,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player,
    const game::ObjectArchetype& product, bool ignorePrerequisites);
[[nodiscard]] const UpgradeDefinition* frozenUpgrade(
    const GameContentSnapshot& content,
    const UpgradeDefinition& requested) noexcept;
[[nodiscard]] bool producerIsSold(
    const ecs::registry& registry, ecs::entity entity) noexcept;
[[nodiscard]] ObjectProductionRequestResult rejected(
    ObjectProductionRejectionReason reason) noexcept;
[[nodiscard]] int64_t calculateUnitCost(
    const game::ObjectArchetype& product, const PlayerState& player,
    const ecs::registry& registry, const ObjectLifecycle& lifecycle);
[[nodiscard]] uint32_t calculateLiveUnitBuildFrames(
    const game::ObjectArchetype& product, const PlayerState& player,
    const ecs::registry& registry, const GameContentSnapshot& content,
    uint32_t framesPerSecond, const EnergySimulationRules& energyRules,
    uint64_t confirmedTick);
[[nodiscard]] uint32_t calculateUpgradeBuildFrames(
    const UpgradeDefinition& upgrade,
    uint32_t framesPerSecond) noexcept;
[[nodiscard]] uint32_t quantityFor(
    const game::ObjectProductionPlan& plan,
    const game::ObjectArchetype& product) noexcept;
[[nodiscard]] bool hasAirfieldQueueCapacity(
    const ecs::registry& registry, ecs::entity producer,
    const ObjectProductionComponent& production,
    const game::ObjectArchetype& requested) noexcept;
[[nodiscard]] uint32_t allocateProductionId(
    ObjectProductionComponent& component) noexcept;

void refundJob(PlayerRegistry& players,
               const ObjectProductionJob& job) noexcept;
void releasePlayerUpgradeReservation(
    PlayerRegistry& players, const ObjectProductionJob& job) noexcept;
void refundAndClear(
    PlayerRegistry& players,
    ObjectProductionComponent& component) noexcept;

[[nodiscard]] std::optional<ObjectProductionExitReservation>
reserveExitRuntime(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ecs::entity entity,
    uint64_t confirmedTick, ObjectProductionExitComponent& runtime);
void releaseExitReservation(
    ObjectProductionExitComponent& runtime,
    ObjectProductionExitReservation reservation) noexcept;
[[nodiscard]] bool commitExitReservation(
    ObjectProductionExitComponent& runtime,
    ObjectProductionExitReservation reservation, ObjectId spawnedObject,
    uint64_t confirmedTick, uint32_t framesPerSecond) noexcept;

[[nodiscard]] ObjectProductionSpawnIntent makeSpawnIntent(
    ObjectId producerId, const OwnerComponent& owner,
    const LogicFixedVec3& producerPosition, math::q32_32 producerYaw,
    const ObjectProductionExitComponent& exitRuntime,
    ObjectProductionExitReservation reservation,
    const ObjectProductionJob& job, uint32_t quantityIndex,
    const game::terrain::TerrainLogic& terrain,
    uint32_t framesPerSecond, const ecs::registry& registry,
    ecs::entity producerEntity);

[[nodiscard]] container::Vector<Candidate> orderedCandidates(
    ecs::registry& registry, const ObjectLifecycle& lifecycle);

} // namespace engine::production_detail

#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/math/fixed/q32_32.h"
#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>

namespace engine {

enum class StrategicAIPhase : uint8_t {
    Bootstrap,
    Economy,
    Base,
    Production,
    Defense,
    Assault,
    Recover,
};

enum class StrategicAIBuildState : uint8_t {
    Unbuilt,
    Reserved,
    Constructing,
    Completed,
    RebuildDelay,
    Exhausted,
};

enum class StrategicAIBuildRole : uint8_t {
    Authored,
    CommandCenter,
    Power,
    Supply,
    Production,
    BaseDefense,
};

struct StrategicAIRuntimeConfig final {
    uint32_t logicFramesPerSecond = 30;
    uint32_t economyIntervalTicks = 15;
    uint32_t structureIntervalTicks = 1;
    uint32_t productionIntervalTicks = 30;
    uint32_t tacticalIntervalTicks = 60;
    uint32_t enemyReviewIntervalTicks = 150;
    uint32_t rebuildDelayTicks = 150;
    uint32_t maximumPlayers = 16;
    uint32_t maximumBuildPlans = 4096;
    math::q32_32 wealthy{7000};
    math::q32_32 poor{2000};
    math::q32_32 structuresWealthyRate{2};
    math::q32_32 structuresPoorRate = math::q32_32::from_fraction(3, 5);
    math::q32_32 teamsWealthyRate{2};
    math::q32_32 teamsPoorRate = math::q32_32::from_fraction(3, 5);
    // AIData.TeamResourcesToStart.  Combat-team production may begin only
    // after the player owns this fraction of the remaining team's estimated
    // cost. Builder recovery and gatherers are deliberately exempt, matching
    // the original AIPlayer team gate rather than turning it into a global
    // production-money check.
    math::q32_32 teamResourcesToStart{};
    // AIData.SkirmishBaseDefenseExtraDistance. Base-defense plans are
    // placed at the maintained base radius plus this authored distance.
    math::q32_32 baseDefenseExtraDistance{150};
};

struct StrategicAIPlayerDescriptor final {
    PlayerId player = INVALID_PLAYER_ID;
    AiDifficulty difficulty = AiDifficulty::None;
    bool autonomousSkirmish = false;
};

struct StrategicAIBuildPlan final {
    uint64_t id = 0;
    PlayerId player = INVALID_PLAYER_ID;
    container::String objectType;
    math::q32_32 anchorX{};
    math::q32_32 anchorY{};
    math::q32_32 yawRadians{};
    container::String scriptName;
    uint32_t sourceSideOrdinal = UINT32_MAX;
    uint32_t sourceBuildListOrdinal = UINT32_MAX;
    StrategicAIBuildState state = StrategicAIBuildState::Unbuilt;
    ObjectId reservedBuilder = INVALID_OBJECT_ID;
    ObjectId constructedObject = INVALID_OBJECT_ID;
    uint64_t nextAttemptTick = 0;
    uint32_t attemptCount = 0;
    // -1 means unlimited rebuilds; positive values are consumed after loss.
    int32_t remainingRebuilds = 0;
    int64_t expectedCost = 0;
    StrategicAIBuildRole role = StrategicAIBuildRole::Authored;
};

struct StrategicAIStructureOption final {
    ObjectId builder = INVALID_OBJECT_ID;
    container::String productType;
    int64_t cost = 0;
    int32_t energyProduction = 0;
    bool supplyCenter = false;
    bool commandCenter = false;
    bool productionFacility = false;
    bool baseDefense = false;
    bool hasAuthoredPlacement = false;
    math::q32_32 authoredOffsetX{};
    math::q32_32 authoredOffsetY{};
    math::q32_32 authoredYawRadians{};
    uint32_t authoredSideOrdinal = UINT32_MAX;
    uint32_t authoredBuildOrdinal = UINT32_MAX;
};

struct StrategicAIProductionOption final {
    ObjectId producer = INVALID_OBJECT_ID;
    container::String productType;
    int64_t cost = 0;
    uint32_t queueDepth = 0;
    bool harvester = false;
    bool builder = false;
    bool combatUnit = false;
};

struct StrategicAIProductionHandle final {
    ObjectId producer = INVALID_OBJECT_ID;
    uint32_t productionId = 0;

    [[nodiscard]] friend constexpr bool operator<(
        const StrategicAIProductionHandle& left,
        const StrategicAIProductionHandle& right) noexcept {
        return left.producer != right.producer
            ? left.producer < right.producer
            : left.productionId < right.productionId;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const StrategicAIProductionHandle&,
        const StrategicAIProductionHandle&) noexcept = default;
};

struct StrategicAIEnemyCandidate final {
    PlayerId player = INVALID_PLAYER_ID;
    math::q32_32 centerX{};
    math::q32_32 centerY{};
    bool hasObjects = false;
    bool hasUnits = false;
    bool hasBuildFacility = false;
};

struct StrategicAITeamProductionOption final {
    ScriptTeamId definition = INVALID_SCRIPT_TEAM_ID;
    container::String name;
    int32_t priority = 0;
    int64_t estimatedCost = 0;
    int32_t maximumInstances = 0;
    uint32_t instanceCount = 0;
    bool conditionSatisfied = false;
    bool buildableWithIdleFactory = false;
    bool assemblyInProgress = false;
};

struct StrategicAITeamReinforcementOption final {
    ScriptTeamId definition = INVALID_SCRIPT_TEAM_ID;
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    container::String productType;
    int32_t priority = 0;
};

struct StrategicAISupplyCenterSnapshot final {
    ObjectId center = INVALID_OBJECT_ID;
    uint32_t desiredGatherers = 0;
    uint32_t assignedGatherers = 0;
    bool hasViableSupply = false;
};

struct StrategicAIPlayerSnapshot final {
    PlayerId player = INVALID_PLAYER_ID;
    int64_t cash = 0;
    int32_t energyProduction = 0;
    int32_t energyConsumption = 0;
    uint32_t ownedStructureCount = 0;
    uint32_t ownedUnitCount = 0;
    uint32_t idleCombatUnitCount = 0;
    uint32_t powerPlantCount = 0;
    uint32_t commandCenterCount = 0;
    uint32_t supplyCenterCount = 0;
    uint32_t productionFacilityCount = 0;
    uint32_t baseDefenseCount = 0;
    uint32_t harvesterCount = 0;
    uint32_t desiredGathererCount = 0;
    uint32_t desiredGatherersPerCenter = 3;
    container::String preferredBaseDefenseStructure;
    bool hasBuilder = false;
    bool hasUsableBuilder = false;
    bool hasBaseAnchor = false;
    bool hasSupplyAnchor = false;
    bool supplyExpansionNeeded = false;
    bool baseThreatened = false;
    math::q32_32 baseAnchorX{};
    math::q32_32 baseAnchorY{};
    math::q32_32 baseRadius{};
    math::q32_32 supplyAnchorX{};
    math::q32_32 supplyAnchorY{};
    ObjectId threatTarget = INVALID_OBJECT_ID;
    ObjectId preferredEnemyTarget = INVALID_OBJECT_ID;
    container::Vector<ObjectId> idleCombatUnits;
    container::Vector<ObjectId> liveCombatUnits;
    container::Vector<ObjectId> liveProducers;
    container::Vector<ObjectId> looseGatherers;
    container::Vector<StrategicAISupplyCenterSnapshot> supplyCenters;
    container::Vector<StrategicAIProductionHandle>
        activeProductionHandles;
    container::Vector<StrategicAIStructureOption> structureOptions;
    container::Vector<StrategicAIProductionOption> productionOptions;
    container::Vector<container::String> scienceOptions;
    container::Vector<StrategicAIEnemyCandidate> enemyCandidates;
    container::Vector<StrategicAITeamProductionOption>
        teamProductionOptions;
    container::Vector<StrategicAITeamReinforcementOption>
        teamReinforcementOptions;
    uint32_t teamPriorityTieBreakIndex = 0;
};

enum class StrategicAIWorkOrderRole : uint8_t {
    BuilderRecovery,
    Gatherer,
    CombatReinforcement,
};

enum class StrategicAIWorkOrderState : uint8_t {
    WaitingForProducer,
    Producing,
    Completed,
    Exhausted,
};

struct StrategicAIWorkOrder final {
    uint64_t id = 0;
    PlayerId player = INVALID_PLAYER_ID;
    StrategicAIWorkOrderRole role =
        StrategicAIWorkOrderRole::CombatReinforcement;
    StrategicAIWorkOrderState state =
        StrategicAIWorkOrderState::WaitingForProducer;
    container::String productType;
    ObjectId producer = INVALID_OBJECT_ID;
    uint32_t productionId = 0;
    uint32_t completedCount = 0;
    uint32_t requiredCount = 1;
    uint32_t failureCount = 0;
    uint64_t nextAttemptTick = 0;
};

enum class StrategicAITeamState : uint8_t {
    Assembling,
    Ready,
    Defending,
    Attacking,
    Recovering,
};

struct StrategicAITeam final {
    uint64_t id = 0;
    PlayerId player = INVALID_PLAYER_ID;
    StrategicAITeamState state = StrategicAITeamState::Assembling;
    ObjectId target = INVALID_OBJECT_ID;
    container::Vector<ObjectId> members;
    uint64_t nextOrderTick = 0;
};

struct StrategicAIPlayerBrain final {
    PlayerId player = INVALID_PLAYER_ID;
    AiDifficulty difficulty = AiDifficulty::None;
    StrategicAIPhase phase = StrategicAIPhase::Bootstrap;
    uint64_t nextEconomyTick = 0;
    uint64_t nextStructureTick = 0;
    uint64_t nextProductionTick = 0;
    uint64_t nextTacticalTick = 0;
    uint64_t nextEnemyReviewTick = 0;
    uint32_t nextSequence = 1;
    uint32_t consecutiveProductionFailures = 0;
    bool autonomousSkirmish = false;
    PlayerId currentEnemy = INVALID_PLAYER_ID;
    ObjectId pendingBridgeRepair = INVALID_OBJECT_ID;
    uint64_t nextBridgeRepairTick = 0;
};

struct StrategicAITeamConditionState final {
    ScriptTeamId definition = INVALID_SCRIPT_TEAM_ID;
    uint64_t nextEvaluationTick = 0;
    bool value = false;
    bool permanentlyUnavailable = false;
};

struct StrategicAIRuntimeSnapshot final {
    static constexpr uint32_t SchemaVersion = 8;

    uint32_t schemaVersion = SchemaVersion;
    StrategicAIRuntimeConfig config;
    container::TreeMap<PlayerId, StrategicAIPlayerBrain> brains;
    container::Vector<StrategicAIBuildPlan> buildPlans;
    container::Vector<StrategicAIWorkOrder> workOrders;
    container::Vector<StrategicAITeam> teams;
    container::Vector<StrategicAITeamConditionState> teamConditions;
    uint64_t nextBuildPlanId = 1;
    uint64_t nextWorkOrderId = 1;
    uint64_t nextTeamId = 1;
};

enum class StrategicAIActionKind : uint8_t {
    BuildStructure,
    ProduceUnit,
    PurchaseScience,
    Defend,
    Attack,
    BuildScenarioTeam,
    ReinforceScenarioTeam,
    AssignGathererDock,
    RepairStructure,
};

struct StrategicAIAction final {
    StrategicAIActionKind kind = StrategicAIActionKind::BuildStructure;
    PlayerId player = INVALID_PLAYER_ID;
    uint32_t sequence = 0;
    uint64_t buildPlanId = 0;
    uint64_t workOrderId = 0;
    // Stable identity of the fallback strategic combat team which emitted an
    // Attack/Defend action. Authored Scenario Teams use ObjectTeamId below;
    // these are deliberately separate ownership domains.
    uint64_t strategicTeamId = 0;
    ObjectId producer = INVALID_OBJECT_ID;
    container::String productType;
    ObjectId target = INVALID_OBJECT_ID;
    container::Vector<ObjectId> actors;
    ScriptTeamId scenarioTeam = INVALID_SCRIPT_TEAM_ID;
    ObjectTeamId objectTeam = INVALID_OBJECT_TEAM_ID;
};

// Player-level persistent planner corresponding to ZH AIPlayer /
// AISkirmishPlayer. It owns only stable values and emits requests through the
// normal GameSession authorities; it never receives an ECS registry.
class StrategicAIRuntime final {
public:
    [[nodiscard]] bool initialize(
        StrategicAIRuntimeConfig config,
        container::Span<const StrategicAIPlayerDescriptor> players);
    void reset() noexcept;

    [[nodiscard]] uint64_t addBuildPlan(StrategicAIBuildPlan plan);
    [[nodiscard]] bool acknowledgeBuildAdmission(
        uint64_t planId, bool accepted, ObjectId builder,
        ObjectId construction, uint64_t retryTick,
        bool permanentFailure) noexcept;
    [[nodiscard]] bool observeBuildObject(
        uint64_t planId, bool present, bool correctOwner,
        bool underConstruction, uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool acknowledgeProduction(
        PlayerId player, uint64_t workOrderId, bool accepted,
        uint32_t productionId, uint64_t retryTick) noexcept;
    [[nodiscard]] bool observeProductionCompletion(
        ObjectId producer, uint32_t productionId,
        bool productionStillActive,
        uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool observeBlockingBridge(
        PlayerId player, ObjectId bridge, uint64_t confirmedTick) noexcept;
    void acknowledgeBridgeRepair(
        PlayerId player, ObjectId bridge, bool stillNeedsRepair,
        uint64_t retryTick) noexcept;
    void acknowledgeTacticalOrder(
        uint64_t strategicTeamId, bool accepted,
        uint64_t retryTick) noexcept;

    void update(
        container::Span<const StrategicAIPlayerSnapshot> snapshots,
        uint64_t confirmedTick,
        container::Vector<StrategicAIAction>& output);

    [[nodiscard]] const StrategicAIPlayerBrain* findBrain(
        PlayerId player) const noexcept;
    [[nodiscard]] StrategicAIBuildPlan* findBuildPlan(
        uint64_t planId) noexcept;
    [[nodiscard]] const container::Vector<StrategicAIBuildPlan>&
        buildPlans() const noexcept { return m_buildPlans; }
    [[nodiscard]] const container::Vector<StrategicAIWorkOrder>&
        workOrders() const noexcept { return m_workOrders; }
    [[nodiscard]] const container::Vector<StrategicAITeam>&
        teams() const noexcept { return m_teams; }
    [[nodiscard]] bool teamSelectionDue(
        PlayerId player, uint64_t confirmedTick) const noexcept;
    [[nodiscard]] bool teamConditionEvaluationDue(
        ScriptTeamId definition, uint64_t confirmedTick) const noexcept;
    void observeTeamCondition(
        ScriptTeamId definition, std::optional<bool> value,
        uint32_t evaluationDelayTicks,
        uint64_t confirmedTick);
    [[nodiscard]] bool teamConditionValue(
        ScriptTeamId definition) const noexcept;
    void acknowledgeScenarioTeamBuild(
        PlayerId player, bool accepted) noexcept;
    [[nodiscard]] uint64_t stableHash() const noexcept;
    [[nodiscard]] bool captureSnapshot(
        StrategicAIRuntimeSnapshot& output) const;
    [[nodiscard]] bool restoreSnapshot(
        const StrategicAIRuntimeSnapshot& snapshot);

private:
    [[nodiscard]] uint32_t nextSequence(
        StrategicAIPlayerBrain& brain) noexcept;
    [[nodiscard]] uint64_t productionDelay(
        const StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot,
        uint32_t failures) const noexcept;
    void updatePhase(
        StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot) noexcept;
    void reconcileTeam(
        StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot,
        uint64_t confirmedTick);
    void reconcileWorkOrders(
        StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot,
        uint64_t confirmedTick);
    void acquireEnemy(
        StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot,
        uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool ensureAutomaticBuildPlan(
        StrategicAIPlayerBrain& brain,
        const StrategicAIPlayerSnapshot& snapshot);
    [[nodiscard]] StrategicAIWorkOrder* ensureWorkOrder(
        StrategicAIPlayerBrain& brain,
        StrategicAIWorkOrderRole role,
        const StrategicAIPlayerSnapshot& snapshot,
        uint64_t confirmedTick);

    StrategicAIRuntimeConfig m_config;
    container::TreeMap<PlayerId, StrategicAIPlayerBrain> m_brains;
    container::Vector<StrategicAIBuildPlan> m_buildPlans;
    container::Vector<StrategicAIWorkOrder> m_workOrders;
    container::Vector<StrategicAITeam> m_teams;
    container::Vector<StrategicAITeamConditionState> m_teamConditions;
    uint64_t m_nextBuildPlanId = 1;
    uint64_t m_nextWorkOrderId = 1;
    uint64_t m_nextTeamId = 1;
};

} // namespace engine

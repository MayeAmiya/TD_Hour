#pragma once

#include <cstdint>
#include <optional>

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"
#include "game/object/contracts/ObjectTeamRegistry.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/player/PlayerTypes.h"
#include "game/scenario/runtime/ScenarioDefinition.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"
#include "math/fixed/q32_32.h"

namespace engine
{

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionAIState;
class GameSessionScriptPresentationState;
struct GameSessionPriorityBuildEntry;

// Script/skirmish/strategic planners that select factories, teams, anchors and
// command targets, then commit through Session lifecycle/order/production
// barriers. Not a forwarding facade: selection, capacity and queue admission
// live here; Session remains the nested spawn/order sink.
class GameSessionScriptScenarioPlanTransactions final
{
public:
    GameSessionScriptScenarioPlanTransactions(GameSessionContentStartState& content,
                                              GameSessionWorldState& world,
                                              GameSessionAIState& ai,
                                              GameSessionScriptPresentationState& presentation,
                                              GameSessionScenarioTransactionPort port) noexcept;

    [[nodiscard]] bool createScriptReinforcementTeam(container::StringView teamName,
                                                     container::StringView destinationWaypointName,
                                                     uint32_t sourceSequence,
                                                     uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptTeam(
        container::StringView teamName, uint32_t sourceSequence,
        uint64_t confirmedTick, bool priorityBuild = true);
    [[nodiscard]] bool reinforceScriptTeam(
        ObjectTeamId team, container::StringView productType,
        uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] bool recruitScriptTeam(container::StringView teamName,
                                         math::q32_32 radius,
                                         uint32_t sourceSequence,
                                         uint64_t confirmedTick);
    [[nodiscard]] bool guardScriptTeamSupplyCenter(ObjectTeamId team,
                                                   int32_t minimumSupplies,
                                                   uint32_t sourceScriptId,
                                                   uint32_t sourceSequence,
                                                   uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptPlayerUpgrade(PlayerId player,
                                                container::StringView upgradeName,
                                                uint32_t sourceSequence,
                                                uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptObjectNearestTeam(PlayerId player,
                                                    ObjectTeamId team,
                                                    container::StringView objectType,
                                                    uint32_t sourceSequence,
                                                    uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptSupplyCenter(PlayerId player,
                                               container::StringView objectType,
                                               int32_t minimumSupplies,
                                               uint32_t sourceSequence,
                                               uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptScenarioBuilding(PlayerId player,
                                                   container::StringView objectType,
                                                   uint32_t sourceSequence,
                                                   uint64_t confirmedTick);
    [[nodiscard]] bool executeScriptSkirmishApproach(ObjectTeamId team,
                                                     container::StringView pathPrefix,
                                                     bool followPath,
                                                     bool asTeam,
                                                     uint32_t sourceScriptId,
                                                     uint32_t sourceSequence,
                                                     uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptPerimeterStructure(PlayerId player,
                                                     container::StringView objectType,
                                                     bool flank,
                                                     bool useFactionBaseDefense,
                                                     uint32_t sourceSequence,
                                                     uint64_t confirmedTick);
    [[nodiscard]] bool fireScriptSpecialPowerAtMostCost(PlayerId player,
                                                        container::StringView specialPower,
                                                        uint32_t sourceScriptId,
                                                        uint32_t sourceSequence,
                                                        uint64_t confirmedTick);
    [[nodiscard]] bool attackScriptNearestValueGroup(ObjectTeamId team,
                                                     script::ScriptComparison comparison,
                                                     int32_t minimumValue,
                                                     uint32_t sourceScriptId,
                                                     uint32_t sourceSequence,
                                                     uint64_t confirmedTick);
    [[nodiscard]] bool executeScriptMostValuableCommandButton(ObjectTeamId team,
                                                              container::StringView buttonName,
                                                              math::q32_32 range,
                                                              bool allTeamMembers,
                                                              uint32_t sourceScriptId,
                                                              uint32_t sourceSequence,
                                                              uint64_t confirmedTick);
    [[nodiscard]] bool setScriptGlobalCombatPolicy(script::ScriptGlobalCombatPolicy policy,
                                                   bool enabled,
                                                   uint64_t confirmedTick);
    [[nodiscard]] size_t sellEverythingForPlayer(PlayerId player, uint64_t confirmedTick);
    [[nodiscard]] bool requestPlayerRepairStructure(PlayerId player, ObjectId structure, uint64_t confirmedTick);
    [[nodiscard]] bool buildScriptObjectNearAnchor(PlayerId player,
                                                   container::StringView objectType,
                                                   math::q32_32 anchorX,
                                                   math::q32_32 anchorY,
                                                   uint32_t sourceSequence,
                                                   uint64_t confirmedTick,
                                                   std::optional<math::q32_32> authoredYawRadians = std::nullopt,
                                                   container::StringView scriptName = {},
                                                   // UINT32_MAX is the "no BuildList identity"
                                                   // sentinel used by ObjectBuilder and
                                                   // ScenarioDefinition; authored ordinals are
                                                   // 0-based, so defaulting to 0 stamped a VALID
                                                   // (side 0, entry 0) pair on construction sites
                                                   // that have no BuildList origin, and completion
                                                   // matched them against the first side's first
                                                   // authored buildIntent — firing a script hook
                                                   // authored for an unrelated structure.
                                                   uint32_t sourceSideOrdinal = UINT32_MAX,
                                                   uint32_t sourceBuildListOrdinal = UINT32_MAX,
                                                   uint64_t strategicPlanId = 0,
                                                   bool authoredBuildList = false,
                                                   int32_t remainingRebuilds = 0);
    void processPriorityBuildEntries();
    void updateScenarioTeamProductions();
    void updateScenarioTeamAssemblies();
    void updatePendingScenarioTeamReinforcements();
    void resolveScenarioReinforcementTransportOrders();
    void resolveScriptContainmentEnterIntents();
    [[nodiscard]] ObjectId recruitScenarioTeamUnit(ObjectTeamId targetTeam,
                                                   const scenario::ScriptTeamDefinition& definition,
                                                   const container::SharedPtr<const game::ObjectArchetype>& wanted,
                                                   const LogicFixedVec3& home,
                                                   math::q32_32 radiusSquared,
                                                   uint32_t& sourceSequence,
                                                   uint64_t confirmedTick,
                                                   uint32_t productionRosterIndex = UINT32_MAX,
                                                   bool orderToHome = true);

private:
    [[nodiscard]] bool tryPriorityBuildEntry(GameSessionPriorityBuildEntry& entry, uint64_t confirmedTick);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionScenarioTransactionPort m_port;
};

} // namespace engine

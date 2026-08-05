#pragma once

#include "core/container/container_types.h"
#include "game/player/PlayerTypes.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/script/GameSessionScriptContracts.h"
#include "game/session/integration/GameSessionScriptConditionEventCursor.h"
#include "game/session/integration/GameSessionScriptLocalPresentationState.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <cstdint>
#include <optional>

namespace engine {
class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
}

namespace engine::script {

// Confirmed script read model. This object owns query-only adapter state and
// implements ScriptWorldQuery directly; GameSessionScriptBridge composes it
// instead of exposing the complete mutable session surface to query code.
class GameSessionScriptQueryPort final : public ScriptWorldQuery {
public:
    GameSessionScriptQueryPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        uint64_t confirmedTick,
        const GameSessionScriptLocalPresentationState& localPresentation);

    [[nodiscard]] std::optional<ScriptWorldObjectSnapshot> findNamedObject(
        container::StringView name) const override;
    [[nodiscard]] std::optional<ScriptWorldObjectSnapshot> resolveObjectSelector(
        const ScriptObjectSelector& selector,
        const ScriptInvocationContext& invocation) const override;
    [[nodiscard]] std::optional<ObjectTeamId> resolveTeamSelector(
        const ScriptTeamSelector& selector,
        const ScriptInvocationContext& invocation) const override;
    [[nodiscard]] ScriptWorldTeamInvocationSet selectConditionTeamInvocation(
        container::Span<const ScriptTeamSelector> candidates) const override;
    // Hook dispatch is prototype-based and may fan out to every active live
    // instance. Newest instances run first, matching RefCode's TeamFactory
    // insertion/list traversal without exposing that storage to Runtime.
    [[nodiscard]] container::Vector<ObjectTeamId> teamHookInstances(
        container::StringView teamName) const override;
    [[nodiscard]] ScriptWorldTeamHookState teamHookState(
        ObjectTeamId team) const noexcept override;
    [[nodiscard]] container::Vector<ScriptWorldTeamUnitDestroyedEvent>
        takeTeamUnitDestroyedHookEvents() const override;
    [[nodiscard]] container::Vector<ScriptWorldObjectHookEvent>
        takeObjectHookEvents() const override;
    [[nodiscard]] ScriptSequentialAuthorityState sequentialObjectState(
        ObjectId object) const noexcept override;
    [[nodiscard]] ScriptSequentialAuthorityState sequentialTeamState(
        ObjectTeamId team) const noexcept override;
    [[nodiscard]] bool teamContained(
        ObjectTeamId team, bool entireTeam) const noexcept override;
    [[nodiscard]] ScriptWorldNamedObjectState namedObjectState(
        container::StringView name) const override;
    [[nodiscard]] ScriptWorldNamedObjectState objectState(
        ObjectId object) const noexcept override;
    [[nodiscard]] bool namedObjectSelected(
        container::StringView name) const noexcept override;
    [[nodiscard]] bool objectSelected(ObjectId object) const noexcept override;
    [[nodiscard]] bool multiplayerOutcome(
        ScriptMultiplayerOutcomeKind kind) const noexcept override;
    [[nodiscard]] bool teamCommandButtonReady(
        container::StringView teamName,
        container::StringView commandButton,
        bool allReady) const override;
    [[nodiscard]] bool teamCommandButtonReady(
        ObjectTeamId team,
        container::StringView commandButton,
        bool allReady) const override;
    [[nodiscard]] std::optional<PlayerId> findPlayer(container::StringView name) const override;
    [[nodiscard]] std::optional<PlayerId> currentEnemyPlayer(
        PlayerId currentPlayer) const noexcept override;
    [[nodiscard]] std::optional<ObjectTeamId> resolveScenarioTeamAlias(
        container::StringView alias,
        ObjectTeamId callingTeam = INVALID_OBJECT_TEAM_ID,
        ObjectTeamId conditionTeam = INVALID_OBJECT_TEAM_ID) const noexcept;
    [[nodiscard]] container::StringView effectiveObjectCommandBarButton(
        ObjectId object, size_t slot) const;
    [[nodiscard]] std::optional<int64_t> playerCash(PlayerId player) const override;
    [[nodiscard]] std::optional<ScriptWorldPlayerEnergySnapshot> playerEnergy(
        PlayerId player) const override;
    [[nodiscard]] std::optional<int32_t> playerSciencePurchasePoints(PlayerId player) const override;
    [[nodiscard]] bool consumePlayerScienceAcquired(
        PlayerId player, container::StringView science) const noexcept override;
    [[nodiscard]] bool playerCanPurchaseScience(
        PlayerId player, container::StringView science) const noexcept override;
    [[nodiscard]] ScriptWorldPlayerObjectSummary playerObjectSummary(
        PlayerId player) const override;
    [[nodiscard]] bool playerHasAnyBuildFacility(
        PlayerId player) const noexcept override;
    [[nodiscard]] ScriptWorldPlayerAreaSummary playerAreaSummary(
        PlayerId player, container::StringView areaName,
        ScriptWorldPlayerAreaMetric metric,
        container::StringView requiredKind = {}) const override;
    [[nodiscard]] std::optional<int64_t> playerObjectTypeCountInArea(
        PlayerId player, container::StringView areaName,
        container::Span<const container::String> objectTypes) const override;
    [[nodiscard]] bool concreteObjectTypeExists(
        container::StringView objectType) const noexcept override;
    [[nodiscard]] int64_t playerObjectTypeCount(
        PlayerId player, container::Span<const container::String> objectTypes,
        bool includeEffectivelyDead) const noexcept override;
    [[nodiscard]] bool techBuildingWithinDistance(
        PlayerId player, container::StringView areaName,
        math::q32_32 extraDistance) const override;
    [[nodiscard]] uint32_t neutralUnmannedObjectCount() const noexcept override;
    [[nodiscard]] std::optional<int32_t> playerStartPosition(PlayerId player) const override;
    [[nodiscard]] std::optional<container::StringView> playerFaction(PlayerId player) const override;
    [[nodiscard]] bool triggerAreaExists(container::StringView areaName) const override;
    [[nodiscard]] std::optional<PlayerId> teamOwner(container::StringView name) const override;
    [[nodiscard]] std::optional<PlayerId> teamOwner(ObjectTeamId team) const override;
    [[nodiscard]] ScriptWorldTeamSummary teamSummary(container::StringView name) const override;
    [[nodiscard]] ScriptWorldTeamSummary teamSummary(ObjectTeamId team) const override;
    [[nodiscard]] std::optional<container::StringView> teamScriptState(
        ObjectTeamId team) const noexcept override;
    [[nodiscard]] bool namedObjectHasAnyStatus(
        container::StringView name, uint64_t statusMask) const override;
    [[nodiscard]] bool objectHasAnyStatus(
        ObjectId object, uint64_t statusMask) const override;
    [[nodiscard]] ScriptWorldTeamStatusSummary teamStatusSummary(
        container::StringView name, uint64_t statusMask) const override;
    [[nodiscard]] ScriptWorldTeamStatusSummary teamStatusSummary(
        ObjectTeamId team, uint64_t statusMask) const override;
    [[nodiscard]] bool namedContainmentIsEmpty(container::StringView name) const override;
    [[nodiscard]] bool objectContainmentIsEmpty(ObjectId object) const override;
    [[nodiscard]] bool namedContainmentHasFreeSlots(container::StringView name) const override;
    [[nodiscard]] bool objectContainmentHasFreeSlots(ObjectId object) const override;
    [[nodiscard]] bool playerSpecialPowerReady(
        PlayerId player, container::StringView specialPower) const noexcept override;
    [[nodiscard]] bool consumeSpecialPowerEvent(
        ScriptSpecialPowerEventPhase phase, PlayerId player,
        container::StringView specialPower,
        ObjectId source = INVALID_OBJECT_ID) const noexcept override;
    [[nodiscard]] bool consumeUpgradeEvent(
        PlayerId player, container::StringView upgrade,
        ObjectId source = INVALID_OBJECT_ID) const noexcept override;
    [[nodiscard]] int64_t playerGarrisonedBuildingCount(
        PlayerId player) const noexcept override;
    [[nodiscard]] int64_t playerCapturedUnitCount(
        PlayerId player) const noexcept override;
    [[nodiscard]] bool playerCanBuildObjectType(
        PlayerId player, container::StringView objectType) const noexcept override;
    [[nodiscard]] bool objectCompletedWaypointPath(
        ObjectId object, container::StringView pathName) const noexcept override;
    [[nodiscard]] bool teamCompletedWaypointPath(
        ObjectTeamId team, container::StringView pathName) const noexcept override;
    [[nodiscard]] bool playerSupplySourceSafe(
        PlayerId player, int32_t minimumSupplies) const noexcept override;
    [[nodiscard]] bool playerSupplySourceAttacked(
        PlayerId player) const noexcept override;
    [[nodiscard]] bool suppliesWithinDistance(
        PlayerId player, container::StringView areaName,
        math::q32_32 extraDistance,
        math::q32_32 minimumValue) const noexcept override;
    [[nodiscard]] bool namedObjectDiscovered(
        container::StringView name, PlayerId observer) const noexcept override;
    [[nodiscard]] bool objectDiscovered(
        ObjectId object, PlayerId observer) const noexcept override;
    [[nodiscard]] bool teamDiscovered(
        container::StringView name, PlayerId observer) const noexcept override;
    [[nodiscard]] bool teamDiscovered(
        ObjectTeamId team, PlayerId observer) const noexcept override;
    [[nodiscard]] bool playerDiscovered(
        PlayerId subject, PlayerId observer) const noexcept override;
    [[nodiscard]] bool namedObjectSeesPlayerByRelationship(
        container::StringView sourceObject, ScriptSightRelationship relationship,
        PlayerId targetPlayer) const noexcept override;
    [[nodiscard]] bool objectSeesPlayerByRelationship(
        ObjectId sourceObject, ScriptSightRelationship relationship,
        PlayerId targetPlayer) const noexcept override;
    [[nodiscard]] bool namedObjectSeesPlayerObjectTypes(
        container::StringView sourceObject, PlayerId targetPlayer,
        container::Span<const container::String> objectTypes) const noexcept override;
    [[nodiscard]] bool objectSeesPlayerObjectTypes(
        ObjectId sourceObject, PlayerId targetPlayer,
        container::Span<const container::String> objectTypes) const noexcept override;
    [[nodiscard]] bool targetLastAttackedByObjectTypes(
        container::StringView target, bool team,
        container::Span<const container::String> objectTypes) const noexcept override;
    [[nodiscard]] bool teamLastAttackedByObjectTypes(
        ObjectTeamId team,
        container::Span<const container::String> objectTypes) const noexcept override;
    [[nodiscard]] bool objectLastAttackedByObjectTypes(
        ObjectId object,
        container::Span<const container::String> objectTypes) const noexcept override;
    [[nodiscard]] bool targetLastAttackedByPlayer(
        container::StringView target, bool team,
        PlayerId player) const noexcept override;
    [[nodiscard]] bool teamLastAttackedByPlayer(
        ObjectTeamId team, PlayerId player) const noexcept override;
    [[nodiscard]] bool objectLastAttackedByPlayer(
        ObjectId object, PlayerId player) const noexcept override;
    [[nodiscard]] bool playerWasAttackedBy(
        PlayerId victim, PlayerId attacker) const noexcept override;
    [[nodiscard]] bool bridgeTransitionObserved(
        container::StringView bridgeObject, bool broken) const noexcept override;
    [[nodiscard]] bool bridgeTransitionObserved(
        ObjectId bridgeObject, bool broken) const noexcept override;
    [[nodiscard]] bool unitEmptied(
        container::StringView objectName) const override;
    [[nodiscard]] bool unitEmptied(ObjectId object) const override;
    [[nodiscard]] bool buildingEnteredByPlayer(
        container::StringView buildingObject,
        PlayerId player) const noexcept override;
    [[nodiscard]] bool buildingEnteredByPlayer(
        ObjectId buildingObject, PlayerId player) const noexcept override;
    [[nodiscard]] bool namedAreaTransition(
        container::StringView objectName, container::StringView areaName,
        ScriptAreaTransitionKind kind) const noexcept override;
    [[nodiscard]] bool objectAreaTransition(
        ObjectId object, container::StringView areaName,
        ScriptAreaTransitionKind kind) const noexcept override;
    [[nodiscard]] bool teamAreaTransition(
        container::StringView teamName, container::StringView areaName,
        uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
        bool entireTeam) const noexcept override;
    [[nodiscard]] bool teamAreaTransition(
        ObjectTeamId team, container::StringView areaName,
        uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
        bool entireTeam) const noexcept override;
    [[nodiscard]] std::optional<math::vec3> teamRadarEventPosition(
        container::StringView name) const override;
    [[nodiscard]] std::optional<math::vec3> teamRadarEventPosition(
        ObjectTeamId team) const override;
    [[nodiscard]] bool namedInsideArea(container::StringView objectName,
                                       container::StringView areaName) const override;
    [[nodiscard]] bool objectInsideArea(
        ObjectId object, container::StringView areaName) const override;
    [[nodiscard]] ScriptWorldTeamAreaSummary teamAreaSummary(
        container::StringView teamName, container::StringView areaName,
        uint8_t allowedSurfaces) const override;
    [[nodiscard]] ScriptWorldTeamAreaSummary teamAreaSummary(
        ObjectTeamId team, container::StringView areaName,
        uint8_t allowedSurfaces) const override;
    [[nodiscard]] bool cameraMovementFinished() const noexcept override;
    [[nodiscard]] bool consumePresentationCompletion(
        ScriptPresentationCompletionKind kind, container::StringView mediaName) const noexcept override;
    [[nodiscard]] bool musicTrackHasCompleted(
        container::StringView trackName, int32_t minimumCompletedLoops) const noexcept override;

private:
    [[nodiscard]] std::optional<PlayerId> resolvePlayer(
        container::StringView name, PlayerId currentPlayer,
        container::StringView currentPlayerAlias) const noexcept;
    [[nodiscard]] std::optional<PlayerId> resolvePlayerAlias(
        container::StringView alias) const noexcept;
    [[nodiscard]] std::optional<PlayerId> currentEnemyPlayerFor(
        PlayerId player) const noexcept;
    [[nodiscard]] bool seesAny(
        ObjectId source, const ObjectSightQuery& query) const;
    [[nodiscard]] bool canReceiveUpgrade(
        ObjectId object, container::StringView upgrade) const;
    [[nodiscard]] std::optional<game::ObjectBuildabilityStatus>
    effectiveObjectBuildability(container::StringView objectType) const noexcept;
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    mutable GameSessionScriptConditionEventCursor m_eventCursor;
    const GameSessionScriptLocalPresentationState& m_localPresentation;
    uint64_t m_confirmedTick = 0;
};

} // namespace engine::script

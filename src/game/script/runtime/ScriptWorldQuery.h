#pragma once

#include "core/container/hash_containers.h"

#include "ScriptProgram.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "core/math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace engine::script {

// This is the detached result supplied by a future ScriptObjectIndex / ECS
// bridge.  It deliberately contains a stable ObjectId and copied transform,
// never an Entity, registry or Object pointer.
struct ScriptWorldObjectHealthSnapshot final {
    // These are Body's current and initial health values, not a presentation
    // percentage and not the authored maximum. `std::nullopt` on the owning
    // object snapshot means no queryable active Body (for example an
    // InactiveBody), which is distinct from a valid Body currently at zero.
    math::q32_32 current{};
    math::q32_32 initial{};
};

// Authoritative position copied from ObjectFixedTransformComponent.  Script
// predicates and any future gameplay effect derived from an object location
// must consume this value. `ScriptWorldObjectSnapshot::position` is retained
// only as the detached presentation projection used by audio/camera/UI
// effects; it must never be converted back into simulation state.
struct ScriptWorldObjectFixedPosition final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

struct ScriptWorldObjectSnapshot final {
    ObjectId id = INVALID_OBJECT_ID;
    ScriptWorldObjectFixedPosition positionFixed{};
    // Presentation-only projection of positionFixed.
    math::vec3 position{};
    PlayerId owner = INVALID_PLAYER_ID;
    std::optional<ScriptWorldObjectHealthSnapshot> health;
    bool alive = false;
};

enum class ScriptWorldNamedObjectState : uint8_t {
    Unknown,
    Alive,
    // The Object still has a live identity/name binding but its Body is
    // effectively dead.  It is created for NAMED_CREATED, destroyed for
    // NAMED_DESTROYED, and not alive for NAMED_NOT_DESTROYED.
    Dying,
    Destroyed,
};

// A compact aggregate keeps ScriptRuntime independent from ECS/terrain while
// still preserving the three legacy Team-area predicates. `considered` is
// after surface/dead/inert filtering; it may be zero for a live empty Team.
struct ScriptWorldTeamAreaSummary final {
    bool teamExists = false;
    bool areaExists = false;
    uint32_t considered = 0;
    uint32_t inside = 0;
};

// A value-only projection of the legacy Team predicates. `createdThisTick`
// is intentionally a pulse, matching Team::m_created after setActive(); it
// is not a synonym for the Team merely existing.
struct ScriptWorldTeamSummary final {
    bool exists = false;
    bool hasUnits = false;
    bool hasObjects = false;
    bool createdThisTick = false;
};

// Compact projection for TEAM_*_HAS_OBJECT_STATUS. `members` counts every
// registry member, matching RefCode's unfiltered Team member iteration;
// `matching` counts members whose ObjectStatus has any requested bit.
struct ScriptWorldTeamStatusSummary final {
    bool exists = false;
    bool membersValid = true;
    uint32_t members = 0;
    uint32_t matching = 0;
};

// A compact player-owned-object projection for the legacy defeat and
// building-count predicates.  The bridge derives it from the authoritative
// ownership index in stable ObjectId order; ScriptRuntime never scans ECS or
// stores a second ownership collection.
struct ScriptWorldPlayerObjectSummary final {
    bool playerExists = false;
    bool hasLegacyCountedObject = false;
    uint32_t structureCount = 0;
    uint32_t victoryStructureCount = 0;
};

enum class ScriptWorldPlayerAreaMetric : uint8_t {
    MatchingKindCount,
    EligibleObjectCount,
    BuildValue,
};

// One bridge scan serves the legacy player-area predicates without exposing
// ECS membership to ScriptRuntime. `value` is either a count or an accumulated
// build cost according to the requested metric.
struct ScriptWorldPlayerAreaSummary final {
    bool playerExists = false;
    bool areaExists = false;
    int64_t value = 0;
};

// Confirmed projection of RefCode Energy. `effectiveProduction` already
// applies the current sabotage window, while consumption remains the authored
// live aggregate. Keeping the integer values lets ScriptRuntime evaluate the
// percentage predicates with cross multiplication instead of introducing a
// new platform-sensitive floating-point division into simulation.
struct ScriptWorldPlayerEnergySnapshot final {
    int32_t effectiveProduction = 0;
    int32_t consumption = 0;
    bool sufficient = false;
};

struct ScriptWorldTeamInvocationSet final {
    bool prototypeExists = false;
    bool multiInstance = false;
    container::Vector<ObjectTeamId> instances;
};

// Detached Team lifecycle/AI projection consumed by the Team-hook scheduler.
// The session bridge owns every expensive or domain-specific decision (Team
// instance lookup, visibility, effective death and AI idleness); Runtime only
// observes these stable values at deterministic dispatch boundaries.
struct ScriptWorldTeamHookState final {
    bool exists = false;
    bool active = false;
    bool createdThisTick = false;
    uint32_t productionActionCount = 0;
    uint32_t productionActionWithoutTeamCount = 0;
    uint32_t totalMemberCount = 0;
    uint32_t aliveMemberCount = 0;
    uint32_t aliveAiMemberCount = 0;
    bool allAliveAiIdle = false;
    bool seesEnemy = false;
    PlayerId owner = INVALID_PLAYER_ID;
};

// One value represents one legacy Team::notifyTeamOfObjectDeath call.  Events
// are intentionally not coalesced: a Team losing three members runs its hook
// three times even when all deaths are committed in one confirmed tick.
struct ScriptWorldTeamUnitDestroyedEvent final {
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
};

// Published only by the Player/Skirmish AI BuildList completion authority.
// The source position selects immutable attachment metadata; the stable
// ObjectId becomes ThisObject. Runtime never infers construction completion
// from ECS or polls production state.
struct ScriptWorldObjectHookEvent final {
    uint32_t sourceSideOrdinal = INVALID_LEGACY_SIDE_ORDINAL;
    uint32_t sourceBuildListOrdinal = INVALID_LEGACY_BUILD_LIST_ORDINAL;
    ObjectId object = INVALID_OBJECT_ID;
};

// Narrow authority snapshot consumed by the Sequential interpreter.  The
// bridge derives it from ObjectLifecycle/ObjectTeamRegistry and the public AI
// state view; Runtime never sees an Entity, AIActorHandle, queue, or group.
struct ScriptSequentialAuthorityState final {
    bool exists = false;
    bool hasAI = false;
    // Guard is a composed Attack + MoveStop operation. This narrow capability
    // projection lets UNIT_GUARD_FOR_FRAMECOUNT preserve RefCode's early
    // return without exposing ObjectAIRuntime storage to ScriptRuntime.
    bool canGuard = false;
    bool idle = false;
    bool effectivelyDead = false;
    PlayerId currentPlayer = INVALID_PLAYER_ID;
};

// Runtime itself owns flags/counters/timers only. Name-to-object and team
// membership belong to the bridge, which can rebuild them from authoritative
// ECS state without ScriptRuntime retaining a competing index.
class ScriptWorldQuery {
public:
    virtual ~ScriptWorldQuery() = default;

    [[nodiscard]] virtual std::optional<ScriptWorldObjectSnapshot> findNamedObject(
        container::StringView name) const = 0;
    [[nodiscard]] virtual std::optional<ScriptWorldObjectSnapshot> resolveObjectSelector(
        const ScriptObjectSelector& selector,
        const ScriptInvocationContext& invocation) const {
        if (selector.kind == ScriptObjectSelector::Kind::ThisObject) {
            static_cast<void>(invocation);
            return std::nullopt;
        }
        return findNamedObject(selector.name);
    }
    [[nodiscard]] virtual std::optional<ObjectTeamId> resolveTeamSelector(
        const ScriptTeamSelector& selector,
        const ScriptInvocationContext& invocation) const {
        static_cast<void>(selector);
        static_cast<void>(invocation);
        return std::nullopt;
    }
    [[nodiscard]] virtual ScriptWorldTeamInvocationSet selectConditionTeamInvocation(
        container::Span<const ScriptTeamSelector> candidates) const {
        static_cast<void>(candidates);
        return {};
    }
    // The bridge returns the current instances newest-first, matching the
    // legacy prototype's prepended intrusive list. Runtime preserves it.
    [[nodiscard]] virtual container::Vector<ObjectTeamId> teamHookInstances(
        container::StringView teamName) const {
        static_cast<void>(teamName);
        return {};
    }
    [[nodiscard]] virtual ScriptWorldTeamHookState teamHookState(
        ObjectTeamId team) const noexcept {
        static_cast<void>(team);
        return {};
    }
    // Consumptive confirmed-tick journal. The const interface follows the
    // existing science-event query: bridge-owned mutable ledgers remain
    // behind the value-only ScriptWorldQuery boundary.
    [[nodiscard]] virtual container::Vector<ScriptWorldTeamUnitDestroyedEvent>
        takeTeamUnitDestroyedHookEvents() const {
        return {};
    }
    [[nodiscard]] virtual container::Vector<ScriptWorldObjectHookEvent>
        takeObjectHookEvents() const {
        return {};
    }
    [[nodiscard]] virtual ScriptSequentialAuthorityState sequentialObjectState(
        ObjectId object) const noexcept {
        static_cast<void>(object);
        return {};
    }
    [[nodiscard]] virtual ScriptSequentialAuthorityState sequentialTeamState(
        ObjectTeamId team) const noexcept {
        static_cast<void>(team);
        return {};
    }
    [[nodiscard]] virtual bool teamContained(
        ObjectTeamId team, bool entireTeam) const noexcept {
        static_cast<void>(team);
        static_cast<void>(entireTeam);
        return false;
    }
    [[nodiscard]] virtual ScriptWorldNamedObjectState namedObjectState(
        container::StringView name) const = 0;
    [[nodiscard]] virtual ScriptWorldNamedObjectState objectState(
        ObjectId object) const noexcept {
        static_cast<void>(object);
        return ScriptWorldNamedObjectState::Unknown;
    }
    [[nodiscard]] virtual bool namedObjectSelected(
        container::StringView name) const noexcept {
        static_cast<void>(name);
        return false;
    }
    [[nodiscard]] virtual bool objectSelected(ObjectId object) const noexcept {
        static_cast<void>(object);
        return false;
    }
    [[nodiscard]] virtual bool multiplayerOutcome(
        ScriptMultiplayerOutcomeKind kind) const noexcept {
        static_cast<void>(kind);
        return false;
    }
    [[nodiscard]] virtual bool teamCommandButtonReady(
        container::StringView teamName,
        container::StringView commandButton,
        bool allReady) const {
        static_cast<void>(teamName);
        static_cast<void>(commandButton);
        static_cast<void>(allReady);
        return false;
    }
    [[nodiscard]] virtual bool teamCommandButtonReady(
        ObjectTeamId team,
        container::StringView commandButton,
        bool allReady) const {
        static_cast<void>(team);
        static_cast<void>(commandButton);
        static_cast<void>(allReady);
        return false;
    }
    // Optional value queries keep small unit-test worlds and non-session
    // tools source-compatible. GameSessionScriptBridge supplies them for
    // compiled map scripts that need authoritative player state.
    [[nodiscard]] virtual std::optional<PlayerId> findPlayer(container::StringView name) const {
        static_cast<void>(name);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<PlayerId> currentEnemyPlayer(
        PlayerId currentPlayer) const noexcept {
        static_cast<void>(currentPlayer);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<int64_t> playerCash(PlayerId player) const {
        static_cast<void>(player);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<ScriptWorldPlayerEnergySnapshot> playerEnergy(
        PlayerId player) const {
        static_cast<void>(player);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<int32_t> playerSciencePurchasePoints(
        PlayerId player) const {
        static_cast<void>(player);
        return std::nullopt;
    }
    // PLAYER_ACQUIRED_SCIENCE consumes precisely one accepted acquisition
    // event.  This mirrors ScriptEngine::isScienceAcquired(..., true), not
    // Player::hasScience: two scripts watching the same acquisition are
    // intentionally source-order competitive.  The const signature matches
    // the other session-owned consumptive query; its implementation keeps the
    // mutable event ledger entirely behind the bridge boundary.
    [[nodiscard]] virtual bool consumePlayerScienceAcquired(
        PlayerId player, container::StringView science) const noexcept {
        static_cast<void>(player);
        static_cast<void>(science);
        return false;
    }
    // PLAYER_CAN_PURCHASE_SCIENCE observes the current authoritative player
    // state and the frozen Science.ini definition.  It is non-consuming so a
    // later same-confirmed-tick script sees effects committed by an earlier
    // script through the bridge.
    [[nodiscard]] virtual bool playerCanPurchaseScience(
        PlayerId player, container::StringView science) const noexcept {
        static_cast<void>(player);
        static_cast<void>(science);
        return false;
    }
    [[nodiscard]] virtual ScriptWorldPlayerObjectSummary playerObjectSummary(
        PlayerId player) const {
        static_cast<void>(player);
        return {};
    }
    [[nodiscard]] virtual bool playerHasAnyBuildFacility(
        PlayerId player) const noexcept {
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual ScriptWorldPlayerAreaSummary playerAreaSummary(
        PlayerId player, container::StringView areaName,
        ScriptWorldPlayerAreaMetric metric,
        container::StringView requiredKind = {}) const {
        static_cast<void>(player);
        static_cast<void>(areaName);
        static_cast<void>(metric);
        static_cast<void>(requiredKind);
        return {};
    }
    [[nodiscard]] virtual std::optional<int64_t> playerObjectTypeCountInArea(
        PlayerId player, container::StringView areaName,
        container::Span<const container::String> objectTypes) const {
        static_cast<void>(player);
        static_cast<void>(areaName);
        static_cast<void>(objectTypes);
        return std::nullopt;
    }
    [[nodiscard]] virtual bool concreteObjectTypeExists(
        container::StringView objectType) const noexcept {
        static_cast<void>(objectType);
        return false;
    }
    [[nodiscard]] virtual int64_t playerObjectTypeCount(
        PlayerId player, container::Span<const container::String> objectTypes,
        bool includeEffectivelyDead) const noexcept {
        static_cast<void>(player);
        static_cast<void>(objectTypes);
        static_cast<void>(includeEffectivelyDead);
        return 0;
    }
    [[nodiscard]] virtual bool techBuildingWithinDistance(
        PlayerId player, container::StringView areaName,
        math::q32_32 extraDistance) const {
        static_cast<void>(player);
        static_cast<void>(areaName);
        static_cast<void>(extraDistance);
        return false;
    }
    [[nodiscard]] virtual uint32_t neutralUnmannedObjectCount() const noexcept {
        return 0;
    }
    [[nodiscard]] virtual std::optional<int32_t> playerStartPosition(PlayerId player) const {
        static_cast<void>(player);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<container::StringView> playerFaction(PlayerId player) const {
        static_cast<void>(player);
        return std::nullopt;
    }
    [[nodiscard]] virtual bool triggerAreaExists(container::StringView areaName) const {
        static_cast<void>(areaName);
        return false;
    }
    [[nodiscard]] virtual std::optional<PlayerId> teamOwner(container::StringView name) const {
        static_cast<void>(name);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<PlayerId> teamOwner(ObjectTeamId team) const {
        static_cast<void>(team);
        return std::nullopt;
    }
    [[nodiscard]] virtual ScriptWorldTeamSummary teamSummary(container::StringView name) const {
        static_cast<void>(name);
        return {};
    }
    [[nodiscard]] virtual ScriptWorldTeamSummary teamSummary(ObjectTeamId team) const {
        static_cast<void>(team);
        return {};
    }
    [[nodiscard]] virtual std::optional<container::StringView> teamScriptState(
        ObjectTeamId team) const noexcept {
        static_cast<void>(team);
        return std::nullopt;
    }
    [[nodiscard]] virtual bool namedObjectHasAnyStatus(
        container::StringView name, uint64_t statusMask) const {
        static_cast<void>(name);
        static_cast<void>(statusMask);
        return false;
    }
    [[nodiscard]] virtual bool objectHasAnyStatus(
        ObjectId object, uint64_t statusMask) const {
        static_cast<void>(object);
        static_cast<void>(statusMask);
        return false;
    }
    [[nodiscard]] virtual ScriptWorldTeamStatusSummary teamStatusSummary(
        container::StringView name, uint64_t statusMask) const {
        static_cast<void>(name);
        static_cast<void>(statusMask);
        return {};
    }
    [[nodiscard]] virtual ScriptWorldTeamStatusSummary teamStatusSummary(
        ObjectTeamId team, uint64_t statusMask) const {
        static_cast<void>(team);
        static_cast<void>(statusMask);
        return {};
    }
    [[nodiscard]] virtual bool namedContainmentIsEmpty(container::StringView name) const {
        static_cast<void>(name);
        return false;
    }
    [[nodiscard]] virtual bool objectContainmentIsEmpty(ObjectId object) const {
        static_cast<void>(object);
        return false;
    }
    [[nodiscard]] virtual bool namedContainmentHasFreeSlots(container::StringView name) const {
        static_cast<void>(name);
        return false;
    }
    [[nodiscard]] virtual bool objectContainmentHasFreeSlots(ObjectId object) const {
        static_cast<void>(object);
        return false;
    }
    [[nodiscard]] virtual bool playerSpecialPowerReady(
        PlayerId player, container::StringView specialPower) const noexcept {
        static_cast<void>(player);
        static_cast<void>(specialPower);
        return false;
    }
    [[nodiscard]] virtual bool consumeSpecialPowerEvent(
        ScriptSpecialPowerEventPhase phase, PlayerId player,
        container::StringView specialPower,
        ObjectId source = INVALID_OBJECT_ID) const noexcept {
        static_cast<void>(phase);
        static_cast<void>(player);
        static_cast<void>(specialPower);
        static_cast<void>(source);
        return false;
    }
    [[nodiscard]] virtual bool consumeUpgradeEvent(
        PlayerId player, container::StringView upgrade,
        ObjectId source = INVALID_OBJECT_ID) const noexcept {
        static_cast<void>(player);
        static_cast<void>(upgrade);
        static_cast<void>(source);
        return false;
    }
    [[nodiscard]] virtual int64_t playerGarrisonedBuildingCount(
        PlayerId player) const noexcept {
        static_cast<void>(player);
        return 0;
    }
    [[nodiscard]] virtual int64_t playerCapturedUnitCount(
        PlayerId player) const noexcept {
        static_cast<void>(player);
        return 0;
    }
    [[nodiscard]] virtual bool playerCanBuildObjectType(
        PlayerId player, container::StringView objectType) const noexcept {
        static_cast<void>(player);
        static_cast<void>(objectType);
        return false;
    }
    [[nodiscard]] virtual bool objectCompletedWaypointPath(
        ObjectId object, container::StringView pathName) const noexcept {
        static_cast<void>(object);
        static_cast<void>(pathName);
        return false;
    }
    [[nodiscard]] virtual bool teamCompletedWaypointPath(
        ObjectTeamId team, container::StringView pathName) const noexcept {
        static_cast<void>(team);
        static_cast<void>(pathName);
        return false;
    }
    [[nodiscard]] virtual bool playerSupplySourceSafe(
        PlayerId player, int32_t minimumSupplies) const noexcept {
        static_cast<void>(player);
        static_cast<void>(minimumSupplies);
        return false;
    }
    [[nodiscard]] virtual bool playerSupplySourceAttacked(
        PlayerId player) const noexcept {
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool suppliesWithinDistance(
        PlayerId player, container::StringView areaName,
        math::q32_32 extraDistance,
        math::q32_32 minimumValue) const noexcept {
        static_cast<void>(player);
        static_cast<void>(areaName);
        static_cast<void>(extraDistance);
        static_cast<void>(minimumValue);
        return false;
    }
    [[nodiscard]] virtual bool namedObjectDiscovered(
        container::StringView name, PlayerId observer) const noexcept {
        static_cast<void>(name);
        static_cast<void>(observer);
        return false;
    }
    [[nodiscard]] virtual bool objectDiscovered(
        ObjectId object, PlayerId observer) const noexcept {
        static_cast<void>(object);
        static_cast<void>(observer);
        return false;
    }
    [[nodiscard]] virtual bool teamDiscovered(
        container::StringView name, PlayerId observer) const noexcept {
        static_cast<void>(name);
        static_cast<void>(observer);
        return false;
    }
    [[nodiscard]] virtual bool teamDiscovered(
        ObjectTeamId team, PlayerId observer) const noexcept {
        static_cast<void>(team);
        static_cast<void>(observer);
        return false;
    }
    [[nodiscard]] virtual bool playerDiscovered(
        PlayerId subject, PlayerId observer) const noexcept {
        static_cast<void>(subject);
        static_cast<void>(observer);
        return false;
    }
    [[nodiscard]] virtual bool namedObjectSeesPlayerByRelationship(
        container::StringView sourceObject, ScriptSightRelationship relationship,
        PlayerId targetPlayer) const noexcept {
        static_cast<void>(sourceObject);
        static_cast<void>(relationship);
        static_cast<void>(targetPlayer);
        return false;
    }
    [[nodiscard]] virtual bool objectSeesPlayerByRelationship(
        ObjectId sourceObject, ScriptSightRelationship relationship,
        PlayerId targetPlayer) const noexcept {
        static_cast<void>(sourceObject);
        static_cast<void>(relationship);
        static_cast<void>(targetPlayer);
        return false;
    }
    [[nodiscard]] virtual bool namedObjectSeesPlayerObjectTypes(
        container::StringView sourceObject, PlayerId targetPlayer,
        container::Span<const container::String> objectTypes) const noexcept {
        static_cast<void>(sourceObject);
        static_cast<void>(targetPlayer);
        static_cast<void>(objectTypes);
        return false;
    }
    [[nodiscard]] virtual bool objectSeesPlayerObjectTypes(
        ObjectId sourceObject, PlayerId targetPlayer,
        container::Span<const container::String> objectTypes) const noexcept {
        static_cast<void>(sourceObject);
        static_cast<void>(targetPlayer);
        static_cast<void>(objectTypes);
        return false;
    }
    [[nodiscard]] virtual bool targetLastAttackedByObjectTypes(
        container::StringView target, bool team,
        container::Span<const container::String> objectTypes) const noexcept {
        static_cast<void>(target);
        static_cast<void>(team);
        static_cast<void>(objectTypes);
        return false;
    }
    [[nodiscard]] virtual bool teamLastAttackedByObjectTypes(
        ObjectTeamId team,
        container::Span<const container::String> objectTypes) const noexcept {
        static_cast<void>(team);
        static_cast<void>(objectTypes);
        return false;
    }
    [[nodiscard]] virtual bool objectLastAttackedByObjectTypes(
        ObjectId object,
        container::Span<const container::String> objectTypes) const noexcept {
        static_cast<void>(object);
        static_cast<void>(objectTypes);
        return false;
    }
    [[nodiscard]] virtual bool targetLastAttackedByPlayer(
        container::StringView target, bool team,
        PlayerId player) const noexcept {
        static_cast<void>(target);
        static_cast<void>(team);
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool teamLastAttackedByPlayer(
        ObjectTeamId team, PlayerId player) const noexcept {
        static_cast<void>(team);
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool objectLastAttackedByPlayer(
        ObjectId object, PlayerId player) const noexcept {
        static_cast<void>(object);
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool playerWasAttackedBy(
        PlayerId victim, PlayerId attacker) const noexcept {
        static_cast<void>(victim);
        static_cast<void>(attacker);
        return false;
    }
    [[nodiscard]] virtual bool bridgeTransitionObserved(
        container::StringView bridgeObject, bool broken) const noexcept {
        static_cast<void>(bridgeObject);
        static_cast<void>(broken);
        return false;
    }
    [[nodiscard]] virtual bool bridgeTransitionObserved(
        ObjectId bridgeObject, bool broken) const noexcept {
        static_cast<void>(bridgeObject);
        static_cast<void>(broken);
        return false;
    }
    [[nodiscard]] virtual bool unitEmptied(
        container::StringView objectName) const {
        static_cast<void>(objectName);
        return false;
    }
    [[nodiscard]] virtual bool unitEmptied(ObjectId object) const {
        static_cast<void>(object);
        return false;
    }
    [[nodiscard]] virtual bool buildingEnteredByPlayer(
        container::StringView buildingObject,
        PlayerId player) const noexcept {
        static_cast<void>(buildingObject);
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool buildingEnteredByPlayer(
        ObjectId buildingObject, PlayerId player) const noexcept {
        static_cast<void>(buildingObject);
        static_cast<void>(player);
        return false;
    }
    [[nodiscard]] virtual bool namedAreaTransition(
        container::StringView objectName, container::StringView areaName,
        ScriptAreaTransitionKind kind) const noexcept {
        static_cast<void>(objectName);
        static_cast<void>(areaName);
        static_cast<void>(kind);
        return false;
    }
    [[nodiscard]] virtual bool objectAreaTransition(
        ObjectId object, container::StringView areaName,
        ScriptAreaTransitionKind kind) const noexcept {
        static_cast<void>(object);
        static_cast<void>(areaName);
        static_cast<void>(kind);
        return false;
    }
    [[nodiscard]] virtual bool teamAreaTransition(
        container::StringView teamName, container::StringView areaName,
        uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
        bool entireTeam) const noexcept {
        static_cast<void>(teamName);
        static_cast<void>(areaName);
        static_cast<void>(allowedSurfaces);
        static_cast<void>(kind);
        static_cast<void>(entireTeam);
        return false;
    }
    [[nodiscard]] virtual bool teamAreaTransition(
        ObjectTeamId team, container::StringView areaName,
        uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
        bool entireTeam) const noexcept {
        static_cast<void>(team);
        static_cast<void>(areaName);
        static_cast<void>(allowedSurfaces);
        static_cast<void>(kind);
        static_cast<void>(entireTeam);
        return false;
    }
    // OBJECT_CREATE_RADAR_EVENT and TEAM_CREATE_RADAR_EVENT have an odd but
    // observable legacy Team rule: a Team must pass Team::hasAnyUnits(), then
    // getEstimateTeamPosition() supplies the *first* member's position (not a
    // centroid).  Keep that compound live-Team query behind the bridge. A
    // null result means missing team, no qualifying live unit, no first-member
    // transform, or an otherwise unusable legacy target; ScriptRuntime then
    // performs the same silent no-op as ScriptActions.
    [[nodiscard]] virtual std::optional<math::vec3> teamRadarEventPosition(
        container::StringView name) const {
        static_cast<void>(name);
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<math::vec3> teamRadarEventPosition(
        ObjectTeamId team) const {
        static_cast<void>(team);
        return std::nullopt;
    }
    // A world with no scripted camera has no active camera movement, matching
    // View's legacy base implementation. GameSessionScriptBridge overrides
    // this with the director's confirmed-transition state.
    [[nodiscard]] virtual bool cameraMovementFinished() const noexcept {
        return true;
    }
    // These queries intentionally consume/read only facts that the owning
    // GameSession admitted at a confirmed-frame ingress. A decoder/audio/UI
    // worker must never call ScriptRuntime directly or use wall-clock timing
    // as a condition result. The default keeps small test worlds and servers
    // conservative until they opt in with a session bridge.
    [[nodiscard]] virtual bool consumePresentationCompletion(
        ScriptPresentationCompletionKind kind, container::StringView mediaName) const noexcept {
        static_cast<void>(kind);
        static_cast<void>(mediaName);
        return false;
    }
    [[nodiscard]] virtual bool musicTrackHasCompleted(
        container::StringView trackName, int32_t minimumCompletedLoops) const noexcept {
        static_cast<void>(trackName);
        static_cast<void>(minimumCompletedLoops);
        return false;
    }
    // Missing object/area deliberately returns false. The legacy
    // NAMED_OUTSIDE_AREA condition is implemented as !NAMED_INSIDE_AREA,
    // therefore it becomes true for a missing object or trigger as well.
    [[nodiscard]] virtual bool namedInsideArea(container::StringView objectName,
                                               container::StringView areaName) const {
        static_cast<void>(objectName);
        static_cast<void>(areaName);
        return false;
    }
    [[nodiscard]] virtual bool objectInsideArea(
        ObjectId object, container::StringView areaName) const {
        static_cast<void>(object);
        static_cast<void>(areaName);
        return false;
    }
    [[nodiscard]] virtual ScriptWorldTeamAreaSummary teamAreaSummary(
        container::StringView teamName, container::StringView areaName,
        uint8_t allowedSurfaces) const {
        static_cast<void>(teamName);
        static_cast<void>(areaName);
        static_cast<void>(allowedSurfaces);
        return {};
    }
    [[nodiscard]] virtual ScriptWorldTeamAreaSummary teamAreaSummary(
        ObjectTeamId team, container::StringView areaName,
        uint8_t allowedSurfaces) const {
        static_cast<void>(team);
        static_cast<void>(areaName);
        static_cast<void>(allowedSurfaces);
        return {};
    }
};

} // namespace engine::script

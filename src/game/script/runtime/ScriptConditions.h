#pragma once

#include "game/script/runtime/ScriptTypes.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"

namespace engine::script
{

// Primitive condition payloads deliberately use names rather than pointers.
// The immutable Program contains authored values only; ScriptRuntime owns the
// matching deterministic counter/flag state.
struct ScriptAlwaysTrueCondition final
{
};
struct ScriptAlwaysFalseCondition final
{
};

struct ScriptCounterCondition final
{
    container::String counter;
    ScriptRuntimeSymbolId counterSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

struct ScriptFlagCondition final
{
    container::String flag;
    ScriptRuntimeSymbolId flagSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    bool expectedValue = true;
};

struct ScriptTimerExpiredCondition final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
};

// RefCode distinguishes a named Object that is still present but effectively
// dead from one whose object pointer has already gone away. `NAMED_CREATED`
// observes the former, `NAMED_NOT_DESTROYED` does not, `NAMED_DYING` matches
// only the former, `NAMED_TOTALLY_DEAD` matches only the remembered latter,
// and `NAMED_DESTROYED` accepts either. Keep that authored distinction in the
// immutable Program; the bridge supplies the actual lifecycle state.
enum class ScriptNamedObjectExpectation : uint8_t
{
    Present,
    Alive,
    Dying,
    TotallyDead,
    Destroyed,
};

struct ScriptNamedObjectStateCondition final
{
    container::String objectName;
    ScriptNamedObjectExpectation expected = ScriptNamedObjectExpectation::Alive;
};

// One of the few legacy conditions that samples client-local presentation
// input. It is forced false for network games; the runtime itself still owns
// only this authored name value, never a selection container.
struct ScriptNamedSelectedCondition final
{
    container::String objectName;
};

enum class ScriptMultiplayerOutcomeKind : uint8_t
{
    AlliedVictory,
    AlliedDefeat,
    PlayerDefeat,
};

struct ScriptMultiplayerOutcomeCondition final
{
    ScriptMultiplayerOutcomeKind kind =
        ScriptMultiplayerOutcomeKind::AlliedVictory;
};

struct ScriptTeamCommandButtonReadyCondition final
{
    container::String teamName;
    container::String commandButton;
    bool allReady = false;
};

// RefCode's UNIT_HEALTH compares the named object's current Body health as a
// percentage of its *initial* Body health.  Keep the integer authored operand
// intact: maps may deliberately compare against values outside 0..100.
struct ScriptUnitHealthCondition final
{
    container::String objectName;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t percent = 0;
};

// RefCode's ObjectStatus predicates test for any bit in the authored mask.
// The compiler resolves the legacy status vocabulary once; runtime retains
// only this detached value and a named Object/Team selector.
struct ScriptObjectStatusCondition final
{
    container::String targetName;
    uint64_t statusMask = 0;
    bool team = false;
    bool entireTeam = false;
};

enum class ScriptNamedContainmentExpectation : uint8_t
{
    Empty,
    HasFreeSlots,
};

struct ScriptNamedContainmentCondition final
{
    container::String objectName;
    ScriptNamedContainmentExpectation expected = ScriptNamedContainmentExpectation::Empty;
};

// Current-state skirmish queries stay detached from ECS.  The bridge resolves
// the frozen SpecialPower content identity and the player's authoritative
// ownership projection at the confirmed script tick.
struct ScriptPlayerSpecialPowerReadyCondition final
{
    container::String player;
    container::String specialPower;
};

struct ScriptSpecialPowerEventCondition final
{
    container::String player;
    container::String specialPower;
    // Empty means any source; FROM_NAMED retains the authored live alias.
    container::String sourceObject;
    ScriptSpecialPowerEventPhase phase = ScriptSpecialPowerEventPhase::Triggered;
};

struct ScriptUpgradeEventCondition final
{
    container::String player;
    container::String upgrade;
    container::String sourceObject;
};

struct ScriptPlayerGarrisonedCountCondition final
{
    container::String player;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t count = 0;
};

struct ScriptPlayerCapturedUnitCountCondition final
{
    container::String player;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t count = 0;
};

struct ScriptSupplySourceSafeCondition final
{
    container::String player;
    int32_t minimumSupplies = 0;
};

struct ScriptSupplySourceAttackedCondition final
{
    container::String player;
};

struct ScriptSuppliesWithinDistanceCondition final
{
    container::String player;
    container::String areaName;
    math::q32_32 extraDistance{};
    math::q32_32 minimumValue{};
};

enum class ScriptDiscoverySubjectKind : uint8_t
{
    NamedObject,
    Team,
    Player,
};

// The observer is always a Player. For Player subjects this preserves the
// slightly counter-intuitive legacy operand order: `subject` owns the objects
// and `observer` is the player whose shroud grid must currently reveal one.
struct ScriptDiscoveryCondition final
{
    container::String subject;
    container::String observer;
    ScriptDiscoverySubjectKind kind = ScriptDiscoverySubjectKind::NamedObject;
};

enum class ScriptSightRelationship : uint8_t
{
    Enemies,
    Neutral,
    Allies,
};

struct ScriptSightedRelationshipCondition final
{
    container::String sourceObject;
    container::String targetPlayer;
    ScriptSightRelationship relationship = ScriptSightRelationship::Enemies;
};

struct ScriptSightedObjectTypeCondition final
{
    container::String sourceObject;
    container::String targetPlayer;
    // May name one ThingTemplate or a runtime ObjectTypeList.
    container::String objectType;
};

enum class ScriptAttackedMatcherKind : uint8_t
{
    ObjectType,
    Player,
};

struct ScriptAttackedCondition final
{
    container::String target;
    container::String matcher;
    ScriptAttackedMatcherKind matcherKind = ScriptAttackedMatcherKind::ObjectType;
    bool team = false;
};

struct ScriptPlayerAttackedByPlayerCondition final
{
    container::String victimPlayer;
    container::String attackerPlayer;
};

struct ScriptBridgeTransitionCondition final
{
    container::String bridgeObject;
    bool broken = false;
};

struct ScriptUnitEmptiedCondition final
{
    container::String objectName;
};

struct ScriptBuildingEnteredCondition final
{
    container::String player;
    container::String buildingObject;
};

struct ScriptAreaTransitionCondition final
{
    container::String target;
    container::String areaName;
    ScriptAreaTransitionKind kind = ScriptAreaTransitionKind::Entered;
    uint8_t allowedSurfaces = 3;
    bool team = false;
    bool entireTeam = false;
};

// View::isCameraMovementFinished() is a first-class legacy script predicate;
// it refers to the logic-owned scripted camera transition, never a renderer
// frame or OS input callback.
struct ScriptCameraMovementFinishedCondition final
{
};

// HAS_FINISHED_VIDEO / HAS_FINISHED_SPEECH / HAS_FINISHED_AUDIO consume one
// validated natural-completion fact from the session's presentation ledger.
// The immutable program retains only the legacy label and category: decoder,
// audio-device and UI state never enter ScriptRuntime.
struct ScriptPresentationCompletionCondition final
{
    ScriptPresentationCompletionKind kind = ScriptPresentationCompletionKind::Video;
    container::String mediaName;
};

// MUSIC_TRACK_HAS_COMPLETED observes the natural-loop count of the current
// active music stream. Unlike the three one-shot conditions above, this is a
// non-consuming threshold check, matching MilesAudioManager.
struct ScriptMusicTrackCompletedCondition final
{
    container::String trackName;
    int32_t minimumCompletedLoops = 0;
};

// RefCode's PLAYER_HAS_CREDITS compares the authored amount to the player's
// current cash (amount < player cash for LESS_THAN), so the runtime retains
// that operand order rather than silently reusing counter-condition wording.
struct ScriptPlayerCashCondition final
{
    container::String player;
    ScriptComparison comparison = ScriptComparison::Equal;
    int64_t value = 0;
};

enum class ScriptPlayerPowerConditionKind : uint8_t
{
    HasSufficientPower,
    HasInsufficientPower,
    SupplyPercent,
    ExcessValue,
};

// Four RefCode Energy predicates share one compact typed condition. For the
// two boolean forms comparison/value are ignored; the ratio and excess forms
// retain the authored integer comparison without leaking Player/Energy into
// the immutable program.
struct ScriptPlayerPowerCondition final
{
    container::String player;
    ScriptPlayerPowerConditionKind kind = ScriptPlayerPowerConditionKind::HasSufficientPower;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

// PLAYER_HAS_SCIENCEPURCHASEPOINTS is a direct threshold check, not a
// science-prerequisite query. It can therefore use PlayerRegistry's existing
// authoritative balance without pulling the science catalog into runtime.
struct ScriptPlayerSciencePurchasePointsCondition final
{
    container::String player;
    int32_t minimumPoints = 0;
};

// PLAYER_ACQUIRED_SCIENCE is deliberately an event predicate, rather than a
// "player currently owns science" state query.  RefCode routes successful
// Player::addScience calls through ScriptEngine::notifyOfAcquiredScience and
// ScriptConditions consumes one matching notification when it evaluates this
// condition.  The session-owned ScriptWorldQuery preserves that one-shot
// behavior without giving ScriptRuntime a mutable PlayerRegistry or event
// queue.
struct ScriptPlayerScienceAcquiredCondition final
{
    container::String player;
    container::String science;
};

// PLAYER_CAN_PURCHASE_SCIENCE is the complementary live-state predicate. It
// does not consume an event: the bridge resolves the immutable Science.ini
// definition and asks its authoritative PlayerRegistry projection whether the
// player can buy it at this exact confirmed-script point.
struct ScriptPlayerCanPurchaseScienceCondition final
{
    container::String player;
    container::String science;
};

// SKIRMISH_PLAYER_HAS_PREREQUISITE_TO_BUILD mirrors Player::canBuild rather
// than asking whether a producer currently has a command button.  The world
// query owns buildability overrides, prerequisite objects and simultaneous
// type limits; ScriptRuntime retains only stable authored selectors.
struct ScriptPlayerCanBuildObjectCondition final
{
    container::String player;
    container::String objectType;
};

// These three predicates share the same authoritative player-object
// projection at runtime.  Keeping the authored maximum separate from the
// projection lets the bridge calculate counts once from the session-owned
// ownership index, rather than teaching ScriptRuntime about ECS entities or
// ThingTemplate storage.
struct ScriptPlayerAllDestroyedCondition final
{
    container::String player;
};

struct ScriptPlayerAllBuildFacilitiesDestroyedCondition final
{
    container::String player;
};

enum class ScriptPlayerBuildingCountKind : uint8_t
{
    AllStructures,
    VictoryStructures,
};

struct ScriptPlayerBuildingCountCondition final
{
    container::String player;
    int32_t maximumCount = 0;
    ScriptPlayerBuildingCountKind kind = ScriptPlayerBuildingCountKind::AllStructures;
};

// Skirmish map predicates whose authoritative data already belongs to the
// session roster/terrain.  These are deliberately normal value conditions:
// no skirmish-AI singleton or UI state is needed to evaluate them.
struct ScriptPlayerStartPositionCondition final
{
    container::String player;
    // Authored positions are 1-based; the session roster stores zero-based
    // layout indices. Keep the authored value intact until evaluation.
    int32_t authoredPosition = 0;
};

struct ScriptPlayerFactionCondition final
{
    container::String player;
    container::String faction;
};

struct ScriptTriggerAreaExistsCondition final
{
    container::String areaName;
};

struct ScriptWaypointPathCompletedCondition final
{
    container::String target;
    container::String pathName;
    bool team = false;
};

enum class ScriptPlayerAreaConditionKind : uint8_t
{
    MatchingKindCount,
    BuildValue,
    HasEligibleObjects,
    HasNoEligibleObjects,
};

struct ScriptPlayerAreaCondition final
{
    container::String player;
    container::String areaName;
    container::String requiredKind;
    ScriptPlayerAreaConditionKind kind = ScriptPlayerAreaConditionKind::HasEligibleObjects;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

// PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA uses exact template
// membership (or an ObjectTypeList), not ThingTemplate::isEquivalentTo.
// Crates are a legacy exception to the dead/INERT filter.
struct ScriptPlayerObjectTypeAreaCountCondition final
{
    container::String player;
    container::String objectType;
    container::String areaName;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

enum class ScriptPlayerObjectTypeCountKind : uint8_t
{
    BuiltByPlayer,
    CurrentComparison,
    LostSincePreviousEvaluation,
};

struct ScriptPlayerObjectTypeCountCondition final
{
    container::String player;
    container::String objectType;
    ScriptPlayerObjectTypeCountKind kind =
        ScriptPlayerObjectTypeCountKind::CurrentComparison;
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

struct ScriptTechBuildingWithinDistanceCondition final
{
    container::String player;
    container::String areaName;
    math::q32_32 extraDistance{};
};

struct ScriptNeutralUnmannedCountCondition final
{
    ScriptComparison comparison = ScriptComparison::Equal;
    int32_t value = 0;
};

struct ScriptNamedObjectOwnerCondition final
{
    container::String objectName;
    container::String player;
};

struct ScriptTeamOwnerCondition final
{
    container::String teamName;
    container::String player;
};

enum class ScriptTeamStateExpectation : uint8_t
{
    HasUnits,
    Destroyed,
    Created,
};

struct ScriptTeamStateCondition final
{
    container::String teamName;
    ScriptTeamStateExpectation expected = ScriptTeamStateExpectation::HasUnits;
};

// TEAM_STATE_IS/IS_NOT compare Team::m_state, an opaque script label. This is
// deliberately distinct from ScriptTeamStateCondition above, which models
// the created/destroyed/has-members lifecycle predicates.
struct ScriptTeamCustomStateCondition final
{
    ScriptTeamSelector team;
    container::String state;
    bool negated = false;
};

enum class ScriptAreaExpectation : uint8_t
{
    Inside,
    Outside,
};

struct ScriptNamedAreaCondition final
{
    container::String objectName;
    container::String areaName;
    ScriptAreaExpectation expected = ScriptAreaExpectation::Inside;
};

enum class ScriptTeamAreaExpectation : uint8_t
{
    // RefCode's misleading "partially" condition is true for any eligible
    // member inside, including an entirely-inside team.
    AnyInside,
    EntirelyInside,
    EntirelyOutside,
};

struct ScriptTeamAreaCondition final
{
    container::String teamName;
    container::String areaName;
    // Legacy SURFACES_ALLOWED: 1=ground, 2=air, 3=ground|air.
    uint8_t allowedSurfaces = 3;
    ScriptTeamAreaExpectation expected = ScriptTeamAreaExpectation::AnyInside;
};

using ScriptCondition = std::variant<ScriptAlwaysTrueCondition,
                                      ScriptAlwaysFalseCondition,
                                      ScriptCounterCondition,
                                      ScriptFlagCondition,
                                      ScriptTimerExpiredCondition,
                                      ScriptNamedObjectStateCondition,
                                      ScriptNamedSelectedCondition,
                                      ScriptMultiplayerOutcomeCondition,
                                      ScriptTeamCommandButtonReadyCondition,
                                      ScriptUnitHealthCondition,
                                      ScriptObjectStatusCondition,
                                      ScriptNamedContainmentCondition,
                                      ScriptPlayerSpecialPowerReadyCondition,
                                      ScriptSpecialPowerEventCondition,
                                      ScriptUpgradeEventCondition,
                                      ScriptPlayerGarrisonedCountCondition,
                                      ScriptPlayerCapturedUnitCountCondition,
                                      ScriptSupplySourceSafeCondition,
                                      ScriptSupplySourceAttackedCondition,
                                      ScriptSuppliesWithinDistanceCondition,
                                      ScriptDiscoveryCondition,
                                      ScriptSightedRelationshipCondition,
                                      ScriptSightedObjectTypeCondition,
                                      ScriptAttackedCondition,
                                      ScriptPlayerAttackedByPlayerCondition,
                                      ScriptBridgeTransitionCondition,
                                      ScriptUnitEmptiedCondition,
                                      ScriptBuildingEnteredCondition,
                                      ScriptAreaTransitionCondition,
                                      ScriptCameraMovementFinishedCondition,
                                      ScriptPresentationCompletionCondition,
                                      ScriptMusicTrackCompletedCondition,
                                      ScriptPlayerCashCondition,
                                      ScriptPlayerPowerCondition,
                                      ScriptPlayerSciencePurchasePointsCondition,
                                      ScriptPlayerScienceAcquiredCondition,
                                      ScriptPlayerCanPurchaseScienceCondition,
                                      ScriptPlayerCanBuildObjectCondition,
                                      ScriptPlayerAllDestroyedCondition,
                                      ScriptPlayerAllBuildFacilitiesDestroyedCondition,
                                      ScriptPlayerBuildingCountCondition,
                                      ScriptPlayerStartPositionCondition,
                                      ScriptPlayerFactionCondition,
                                      ScriptTriggerAreaExistsCondition,
                                      ScriptWaypointPathCompletedCondition,
                                      ScriptPlayerAreaCondition,
                                      ScriptPlayerObjectTypeAreaCountCondition,
                                      ScriptPlayerObjectTypeCountCondition,
                                      ScriptTechBuildingWithinDistanceCondition,
                                      ScriptNeutralUnmannedCountCondition,
                                      ScriptNamedObjectOwnerCondition,
                                      ScriptTeamOwnerCondition,
                                      ScriptTeamStateCondition,
                                      ScriptTeamCustomStateCondition,
                                      ScriptNamedAreaCondition,
                                      ScriptTeamAreaCondition>;

// One clause represents an AND chain. ScriptDefinition::anyOf is the outer
// OR list, matching RefCode's OrCondition -> Condition structure.
struct ScriptAndClause final
{
    container::Vector<ScriptCondition> allOf;
};

} // namespace engine::script

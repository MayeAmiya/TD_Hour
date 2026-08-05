#pragma once

#include "game/script/runtime/ScriptTypes.h"

namespace engine::script
{

// Script-issued unit commands are authored as immutable names/coordinates;
// ScriptRuntime resolves named actors through ScriptWorldQuery and emits a
// value-only effect. They never become GameCommand packets, which keeps
// confirmed script behavior out of player replay/network ingress.
enum class ScriptOrderKind : uint8_t
{
    Move,
    // Stop is an interruption/clear operation. It is admitted synchronously
    // and never retained as an unconsumed queue head.
    Stop,
    Attack,
    Build,
    CommandButton,
    SpecialPower,
    // Script-only tactical wrappers remain outside GameCommand. Hunt and the
    // first Guard slice carry no authored target; squad/area attacks retain
    // their authored non-object target explicitly.
    TacticalAttack,
};

enum class ScriptMoveRouteSubtype : uint8_t
{
    Direct,
    WaypointPathIndividuals,
    WaypointPathTeam,
    WaypointPathIndividualsExact,
    WaypointPathTeamExact,
    WanderWaypointPath,
    PanicWaypointPath,
};

enum class ScriptTacticalAttackSubtype : uint8_t
{
    None,
    Hunt,
    Guard,
    // Legacy TEAM_GUARD_IN_TUNNEL_NETWORK keeps the Guard state family but
    // uses the tunnel-network policy (enter/exit and nemesis handoff).
    GuardTunnelNetwork,
    AttackSquad,
    AttackArea,
};

// Keep the authored selector typed. A Team name is not an Object name: it is
// resolved through ScenarioDefinition -> ObjectTeamRegistry only at the
// session bridge, after the current map has supplied its live team members.
enum class ScriptOrderActorSelector : uint8_t
{
    NamedObjects,
    ScenarioTeam,
    // PLAYER_HUNT expands the player's live ownership index at the stamped
    // GameSession boundary. It still enters OrderExecutor as NamedObjects;
    // this selector only preserves the authored namespace until then.
    PlayerAssets,
};

struct ScriptIssueOrderAction final
{
    ScriptOrderKind kind = ScriptOrderKind::Move;
    ScriptMoveRouteSubtype moveRouteSubtype = ScriptMoveRouteSubtype::Direct;
    ScriptTacticalAttackSubtype tacticalAttackSubtype =
        ScriptTacticalAttackSubtype::None;
    ScriptOrderActorSelector actorSelector = ScriptOrderActorSelector::NamedObjects;
    container::Vector<container::String> actorNames;
    container::String teamName;
    container::String playerName;
    container::String targetObjectName;
    // Tactical squad/area targets are distinct authored namespaces. Never
    // infer either from targetObjectName.
    container::String targetTeamName;
    container::String targetAreaName;
    std::optional<ScriptFixedVec3> targetPosition;
    // Direct carries one authored waypoint name. WaypointPath* carries the
    // legacy waypoint-path label. The compiler has no TerrainLogic; both
    // remain authored strings through Script transport.
    container::String targetWaypointName;
    container::String contentName;
    // NAMED_ATTACK_NAMED uses RefCode's aiForceAttackObject path, while
    // TEAM_ATTACK_NAMED uses the ordinary groupAttackObject path. Preserve
    // this targeting policy even though the current direct-hit combat slice
    // has no pursuit/retargeting consumer yet.
    bool forceAttack = false;
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;
    // TEAM_STOP_AND_DISBAND performs its Stop first, then moves the stable
    // member snapshot into the controlling player's default Team.
    bool disbandAfterStop = false;
    bool queued = false;
};

// This legacy action is deliberately separate from a generic Attack order.
// It selects one specially-authored live Weapon slot, fires at the launcher's
// own position, and transfers the resulting ProjectileObject to a waypoint
// route.  Runtime retains authored names only; GameSession owns both live
// weapon state and TerrainLogic resolution.
struct ScriptFireWeaponFollowingWaypointPathAction final
{
    container::String objectName;
    container::String waypointPathName;
};

// BUILD_TEAM names a Scenario Team prototype rather than an already-live
// instance. The GameSession production planner materializes the inactive
// Team and binds every admitted factory job to that stable Team ID.
struct ScriptBuildTeamAction final
{
    container::String teamName;
};

struct ScriptGuardSupplyCenterAction final
{
    container::String teamName;
    int32_t minimumSupplies = 0;
};

// CREATE_REINFORCEMENT_TEAM creates a fresh runtime instance from a Scenario
// Team prototype.  It must retain the authored prototype name until the
// GameSession boundary: resolving a current ObjectTeamId here would incorrectly
// reuse the first non-singleton instance and would lose the immutable roster.
struct ScriptCreateReinforcementTeamAction final
{
    container::String teamName;
    container::String destinationWaypointName;
};

struct ScriptRecruitTeamAction final
{
    container::String teamName;
    math::q32_32 radius{};
};

// CommandButton actions are not generic AI orders.  The authored button is
// resolved through the session's frozen CommandButton content and dispatched
// to the subsystem that owns its Command (special power, production, sale,
// combat, ...).  Keeping the target shape explicit prevents an unsupported
// button from becoming an unconsumed ObjectOrderQueue head.
enum class ScriptCommandButtonTargetKind : uint8_t
{
    None,
    NamedObject,
    Waypoint,
    WaypointPath,
    NearestEnemyUnit,
    NearestGarrisonedBuilding,
    NearestKindOf,
    NearestEnemyBuilding,
    NearestEnemyBuildingClass,
    NearestObjectType,
    // Skirmish planner target: highest current build value inside an
    // authored radius around the Team center, with stable distance/ID ties.
    MostValuableEnemy,
};

enum class ScriptCommandButtonActorPolicy : uint8_t
{
    All,
    PartialUsable,
};

struct ScriptUseCommandButtonAction final
{
    ScriptOrderActorSelector actorSelector =
        ScriptOrderActorSelector::NamedObjects;
    container::Vector<container::String> actorNames;
    container::String teamName;
    container::String buttonName;
    ScriptCommandButtonActorPolicy actorPolicy =
        ScriptCommandButtonActorPolicy::All;
    math::q32_32 actorPercentage{int32_t{100}};
    bool preselectSourceAndTarget = false;
    ScriptCommandButtonTargetKind targetKind =
        ScriptCommandButtonTargetKind::None;
    container::String targetObjectName;
    container::String targetWaypointName;
    // Canonical KindOf token or ThingTemplate/ObjectTypeList name, depending
    // on targetKind. Runtime resolves mutable ObjectTypeList membership before
    // the effect crosses into GameSession.
    container::String targetFilter;
};

enum class ScriptFacingTargetKind : uint8_t
{
    NamedObject,
    Waypoint,
};

// The legacy Face actions are immediate script commands, not persistent
// attack/move orders. Keep their actor namespace and target namespace typed
// until Runtime/bridge resolution instead of encoding either as a generic
// string command.
struct ScriptFacingAction final
{
    ScriptOrderActorSelector actorSelector =
        ScriptOrderActorSelector::NamedObjects;
    container::Vector<container::String> actorNames;
    container::String teamName;
    ScriptFacingTargetKind targetKind =
        ScriptFacingTargetKind::NamedObject;
    container::String targetName;
};

enum class ScriptAIBehaviorMutationKind : uint8_t
{
    ApplyAttackPrioritySet,
    SetAttitude,
    SetCommandButtonHunt,
    IncreaseTeamProductionPriority,
    DecreaseTeamProductionPriority,
    WanderInPlace,
};

enum class ScriptAIBehaviorTargetKind : uint8_t
{
    NamedObject,
    ScenarioTeam,
};

// Named and Team variants deliberately share one typed action.  A named
// Object is resolved to ObjectId by ScriptRuntime, while Team membership is
// expanded at stamped bridge application time so preceding ownership effects
// in the same script pass remain observable.
struct ScriptAIBehaviorMutationAction final
{
    ScriptAIBehaviorTargetKind targetKind =
        ScriptAIBehaviorTargetKind::NamedObject;
    container::String targetName;
    ScriptAIBehaviorMutationKind mutation =
        ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet;
    container::String attackPrioritySet;
    container::String commandButton;
    int32_t attitude = 0;
};

enum class ScriptAttackPriorityMutationKind : uint8_t
{
    ObjectType,
    KindOf,
    Default,
};

// Attack priority sets are mutable ScriptEngine state in RefCode.  Keep the
// authored object-list/KindOf name until runtime so SET_ATTACK_PRIORITY_THING
// observes object-type list edits made by earlier actions in the same pass.
struct ScriptAttackPriorityMutationAction final
{
    ScriptAttackPriorityMutationKind mutation =
        ScriptAttackPriorityMutationKind::Default;
    container::String setName;
    container::String selector;
    int32_t priority = 1;
};

enum class ScriptStoppingDistanceTargetKind : uint8_t
{
    NamedObject,
    ScenarioTeam,
};

struct ScriptStoppingDistanceAction final
{
    ScriptStoppingDistanceTargetKind targetKind =
        ScriptStoppingDistanceTargetKind::NamedObject;
    container::String targetName;
    math::q32_32 distance{};
};

struct ScriptMoveTowardsNearestAction final
{
    ScriptOrderActorSelector actorSelector = ScriptOrderActorSelector::NamedObjects;
    container::String actorName;
    container::String teamName;
    container::String objectType;
    container::String triggerArea;
};

enum class ScriptSpecialPowerCountdownOperation : uint8_t
{
    Pause,
    Set,
    Add,
};

struct ScriptSpecialPowerCountdownAction final
{
    ScriptSpecialPowerCountdownOperation operation =
        ScriptSpecialPowerCountdownOperation::Pause;
    container::String objectName;
    container::String specialPower;
    int32_t seconds = 0;
    bool paused = false;
};

struct ScriptWarehouseValueAction final
{
    container::String objectName;
    int32_t cashValue = 0;
};

struct ScriptCaveIndexAction final
{
    container::String objectName;
    int32_t caveIndex = 0;
};

} // namespace engine::script

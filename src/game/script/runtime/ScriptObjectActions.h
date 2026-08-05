#pragma once

#include "core/math/fixed/q32_32.h"
#include "game/script/runtime/ScriptTypes.h"

#include <optional>

namespace engine::script
{

// All legacy object-spawn opcodes share this immutable request. The
// compiler/runtime retain authored names and coordinates only; resolving a
// Scenario Team, terrain waypoint, frozen archetype and ObjectId belongs to
// the stamped GameSession bridge. An explicit coordinate and a waypoint are
// mutually exclusive by construction. `position` is a world transform (not
// a terrain-relative map-object offset), matching ScriptActions::doCreateObject.
struct ScriptCreateObjectAction final
{
    container::String objectName;
    container::String templateName;
    container::String teamName;
    std::optional<ScriptFixedVec3> position;
    container::String waypointName;
    math::q32_32 rotation{};
};

// DELETE and KILL have different death/presentation behavior in RefCode, but
// both remove the object from ScriptEngine's named-live lookup. Until body and
// death modules exist, preserve their shared lifecycle intent explicitly.
struct ScriptDestroyNamedObjectAction final
{
    container::String objectName;
    bool forceKill = false;
};

enum class ScriptLifecycleTargetKind : uint8_t
{
    ScenarioTeam,
    Player,
};

enum class ScriptLifecycleOperation : uint8_t
{
    Delete,
    DeleteLiving,
    Kill,
};

// Team/Player lifecycle actions retain their authored selector until the
// stamped bridge boundary. Membership and ownership can change earlier in the
// same script pass, so freezing ObjectIds in the compiler/runtime would be
// observably wrong.
struct ScriptLifecycleAction final
{
    ScriptLifecycleTargetKind targetKind = ScriptLifecycleTargetKind::ScenarioTeam;
    ScriptLifecycleOperation operation = ScriptLifecycleOperation::Delete;
    container::String targetName;
};

enum class ScriptContainmentActionKind : uint8_t
{
    EjectContainerContents,
    EjectSpecificStructure,
    EjectTeamContainerContents,
    DetachNamedOccupant,
    DetachTeamOccupants,
    EjectPlayerStructures,
    KillContainerContents,
    SetEvacuationDisposition,
};

// Exit actions are containment transactions, not movement approximations.
// Named actors are resolved by ScriptRuntime; Team/Player membership remains
// live until the bridge applies the stamped effect.
struct ScriptContainmentAction final
{
    ScriptContainmentActionKind kind = ScriptContainmentActionKind::EjectContainerContents;
    container::String targetName;
    int32_t evacuationDisposition = 0;
};

enum class ScriptContainmentEnterActionKind : uint8_t
{
    LoadTeamTransports,
    TeamCaptureNearestUnmanned,
    NamedEnterNamed,
    TeamEnterNamed,
    TeamGarrisonSpecific,
    TeamGarrisonNearest,
    NamedGarrisonSpecific,
    NamedGarrisonNearest,
    PlayerGarrisonAll,
};

struct ScriptContainmentEnterAction final
{
    ScriptContainmentEnterActionKind kind =
        ScriptContainmentEnterActionKind::NamedEnterNamed;
    ScriptObjectSelector object;
    ScriptTeamSelector team;
    container::String player;
    ScriptObjectSelector container;
};

// Ownership changes remain an explicit Session transaction.  Named-object
// transfer moves one Object into the destination player's default Team;
// Scenario-Team transfer changes the live Team owner and updates all current
// members atomically.  Neither selector is resolved by the compiler.
enum class ScriptOwnershipTransferSelector : uint8_t
{
    NamedObject,
    ScenarioTeam,
    PlayerAssets,
    MergeScenarioTeam,
};

struct ScriptTransferOwnershipAction final
{
    ScriptOwnershipTransferSelector selector = ScriptOwnershipTransferSelector::NamedObject;
    container::String objectName;
    container::String teamName;
    container::String targetTeamName;
    container::String sourcePlayer;
    container::String targetPlayer;
};

// RefCode exposes two directly consumable scripted damage actions.  Keep the
// authored selector explicit instead of collapsing a Scenario Team name into
// a transient object list in the compiler: team membership is live session
// state and must be resolved only at stamped bridge application time.
enum class ScriptDamageTargetSelector : uint8_t
{
    NamedObject,
    ScenarioTeam,
};

struct ScriptDamageAction final
{
    ScriptDamageTargetSelector targetSelector = ScriptDamageTargetSelector::NamedObject;
    container::String objectName;
    container::String teamName;
    // NAMED_DAMAGE and non-negative DAMAGE_MEMBERS_OF_TEAM both use
    // UNRESISTABLE/NORMAL damage in RefCode.  A negative team amount calls
    // Object::kill(), represented by forceKill rather than a magic amount.
    // The legacy compiler quantizes the authored map value once. Script
    // runtime actions are authoritative state and therefore fixed-only.
    math::q32_32 amount{};
    bool forceKill = false;
};

struct ScriptGrantObjectUpgradeAction final
{
    container::String objectName;
    container::String upgradeName;
};

enum class ScriptObjectStateTargetKind : uint8_t
{
    NamedObject,
    ScenarioTeam,
};

enum class ScriptObjectStateMutationKind : uint8_t
{
    Held,
    Repulsor,
    Unmanned,
    RailroadHeld,
    StealthEnabled,
    PanelEnabled,
    PanelPowered,
    PanelIndestructible,
    PanelUnsellable,
    PanelSelectable,
    PanelAiRecruitable,
    PanelPlayerTargetable,
    // Team::setRecruitable is a Team-instance policy, not the per-object AI
    // recruitable flag exposed by Object Panel actions.
    TeamRecruitable,
};

// Script-owned Held and Repulsor mutations already have authoritative modern
// ObjectDisabled/ObjectStatus storage. Named targets are resolved to a stable
// ObjectId by the runtime; Team membership remains live until the stamped
// bridge boundary, matching RefCode's execute-time Team traversal.
struct ScriptObjectStateMutationAction final
{
    ScriptObjectStateTargetKind targetKind = ScriptObjectStateTargetKind::NamedObject;
    container::String targetName;
    ScriptObjectStateMutationKind mutation = ScriptObjectStateMutationKind::Held;
    bool enabled = false;
};

enum class ScriptGlobalObjectOperation : uint8_t
{
    IdleHumanUnits,
    ResumeHumanSupplyTrucking,
    DeleteAllUnmanned,
};

struct ScriptGlobalObjectAction final
{
    ScriptGlobalObjectOperation operation = ScriptGlobalObjectOperation::IdleHumanUnits;
};

struct ScriptBoobyTrapAction final
{
    ScriptObjectStateTargetKind targetKind = ScriptObjectStateTargetKind::NamedObject;
    container::String targetName;
    container::String templateName;
};

struct ScriptToppleDirectionAction final
{
    container::String objectName;
    ScriptFixedVec3 direction{};
};

// Object type lists belong to ScriptEngine state rather than the world. They
// are mutated directly by ScriptRuntime and therefore emit no external
// effect. RefCode preserves insertion order, ignores duplicate additions and
// removes the list itself when its last member is removed.
struct ScriptModifyObjectTypeListAction final
{
    container::String listName;
    container::String objectType;
    bool add = true;
};

} // namespace engine::script

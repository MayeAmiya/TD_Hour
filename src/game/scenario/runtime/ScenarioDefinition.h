#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include "game/player/MatchSetup.h"

#include <cstdint>
#include <optional>
namespace engine::scenario {

// Original SidesList/Team dictionaries contain a great deal of script data
// that the ECS phase must not silently discard.  These values are immutable
// source data; no script or entity behavior runs from this layer yet.
struct RawScenarioField final {
    container::String key;
    container::String value;
};

struct ScenarioPlayerBinding final {
    // `scenarioName` is the exact playerName stored by the map SidesList
    // (for example PlyrChina or Player_3).  It is intentionally separate
    // from the command/lobby slot namespace represented by PlayerId.
    container::String scenarioName;
    PlayerId player = INVALID_PLAYER_ID;
    // These are the value fields consumed by PlayerRegistry while it
    // materializes map-owned AI/civilian players.  Keeping the original
    // faction string is important: campaign maps are authored against a
    // PlayerTemplate name, not a frozen catalog ID.
    container::String factionTemplateName;
    container::String displayName;
    bool isHuman = false;
    container::Vector<RawScenarioField> fields;
};

// This is the stable source position of one SidesList entry, counted across
// CkMp SidesList chunks in file order.  It deliberately survives even when
// two entries share the same playerName: RefCode binds ScriptList execution
// by Side position, while its ordinary name lookup returns the first match.
inline constexpr uint32_t INVALID_SCENARIO_SIDE_ORDINAL = 0xffffffffu;
inline constexpr uint32_t INVALID_SCENARIO_BUILD_LIST_ORDINAL = 0xffffffffu;

struct ScenarioSideBinding final {
    uint32_t sourceSideOrdinal = INVALID_SCENARIO_SIDE_ORDINAL;
    container::String scenarioName;
    // Neutral is a valid side owner here.  `INVALID_PLAYER_ID` is not.
    PlayerId player = INVALID_PLAYER_ID;
};

struct ScenarioTeamUnitPlan final {
    container::String templateName;
    uint32_t minimumUnits = 0;
    uint32_t maximumUnits = 0;
};

// Immutable projection of TeamTemplateInfo fields needed by reinforcement,
// recruitment and Player-level Team production. Runtime systems consume this
// typed value and never reinterpret RawScenarioField strings per tick.
struct ScenarioTeamPlan final {
    container::Vector<ScenarioTeamUnitPlan> units;
    container::String homeWaypoint;
    container::String onCreateScript;
    container::String productionCondition;
    container::String reinforcementTransport;
    container::String reinforcementOriginWaypoint;
    int32_t initialIdleFrames = 0;
    int32_t productionPriority = 0;
    int32_t productionPrioritySuccessIncrease = 0;
    int32_t productionPriorityFailureDecrease = 0;
    int32_t veterancy = 0;
    // SidesList teamAggressiveness maps directly to ZH AttitudeType
    // (-2 Sleep .. +2 Aggressive). Keep the scenario layer value-only.
    int32_t initialAttitude = 0;
    bool aiRecruitable = false;
    bool automaticallyReinforce = false;
    bool attackCommonTarget = false;
    bool executesActionsOnCreate = false;
    bool startsFull = false;
    bool transportsExit = false;
};

struct ScriptTeamDefinition final {
    ScriptTeamId id = INVALID_SCRIPT_TEAM_ID;
    container::String name;
    container::String ownerAlias;
    PlayerId resolvedOwner = INVALID_PLAYER_ID;
    // A SidesList Team is a prototype, not automatically a live unit group.
    // TeamFactory creates one inactive instance only for singleton prototypes;
    // ordinary prototypes receive their first instance when a map object,
    // script, production system, or reinforcement path asks for one.
    bool isSingleton = false;
    // RefCode's script condition driver also treats a prototype with fewer
    // than two permitted instances as a singleton, even when the explicit
    // teamIsSingleton bit is clear.  Preserve the authored value so the
    // ScriptRuntime/bridge can choose the correct one-shot/ELSE semantics
    // without interpreting the raw SidesList dictionary at runtime.
    int32_t maximumInstances = 0;
    // `team<playerName>` is the Player's default Team in RefCode.  It is an
    // alias for the PlayerDefault ObjectTeam instance, never an extra group.
    bool isPlayerDefault = false;
    ScenarioTeamPlan plan;
    container::Vector<RawScenarioField> fields;
};

// This is data only.  A future production/AI layer will interpret it after
// Object lifecycle and ownership transfer have one authoritative ECS path.
struct ScenarioBuildIntent final {
    // Stable authored identity of this BuildList entry.  The list ordinal is
    // local to its Side and survives duplicate structure/object names.
    uint32_t sourceSideOrdinal = INVALID_SCENARIO_SIDE_ORDINAL;
    uint32_t sourceBuildListOrdinal = INVALID_SCENARIO_BUILD_LIST_ORDINAL;
    container::String structureName;
    container::String templateName;
    // RefCode stores this on BuildListInfo and invokes it only after the AI
    // reports the corresponding structure complete.  Scenario data freezes
    // the authored name; it does not manufacture that production event.
    container::String objectScriptAttachment;
    container::String ownerAlias;
    // SidesList BuildList belongs directly to the owning Side, not to a
    // later textual alias lookup.  Preserve the resolved source owner so
    // duplicate playerName entries remain distinguishable to a future AI /
    // build-plan runtime.
    PlayerId resolvedOwner = INVALID_PLAYER_ID;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 angle{};
    bool fixedPoseValid = false;
    bool initiallyBuilt = false;
    container::Vector<RawScenarioField> fields;
};

struct ScenarioDiplomacyOverride final {
    PlayerId from = INVALID_PLAYER_ID;
    PlayerId to = INVALID_PLAYER_ID;
    PlayerRelationship relationship = PlayerRelationship::Neutral;
};

enum class OwnerReferenceKind : uint8_t {
    Unknown,
    Player,
    ScriptTeam,
};

struct OwnerReference final {
    OwnerReferenceKind kind = OwnerReferenceKind::Unknown;
    PlayerId player = INVALID_PLAYER_ID;
    ScriptTeamId scriptTeam = INVALID_SCRIPT_TEAM_ID;

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return kind != OwnerReferenceKind::Unknown;
    }
};

struct ScenarioValidationIssue final {
    container::String message;
};

// Modern immutable equivalent of the relevant SidesList data.  It separates
// player aliases, multiplayer diplomacy, script teams and initial build data;
// in particular, a ScriptTeam is never misused as a multiplayer alliance.
class ScenarioDefinition final {
public:
    // Builder-only mutation.  Once finalized, source data is frozen so alias
    // caches and canonical ScriptTeam IDs cannot silently become stale.
    [[nodiscard]] bool addPlayerBinding(ScenarioPlayerBinding binding);
    [[nodiscard]] bool addSideBinding(ScenarioSideBinding binding);
    [[nodiscard]] bool addScriptTeam(ScriptTeamDefinition definition);
    [[nodiscard]] bool addBuildIntent(ScenarioBuildIntent intent);
    [[nodiscard]] bool addDiplomacyOverride(ScenarioDiplomacyOverride overrideValue);

    [[nodiscard]] container::Span<const ScenarioPlayerBinding> players() const noexcept {
        return m_players;
    }
    [[nodiscard]] container::Span<const ScenarioSideBinding> sides() const noexcept {
        return m_sides;
    }
    [[nodiscard]] container::Span<const ScriptTeamDefinition> scriptTeams() const noexcept {
        return m_scriptTeams;
    }
    [[nodiscard]] container::Span<const ScenarioBuildIntent> buildIntents() const noexcept {
        return m_buildIntents;
    }
    [[nodiscard]] container::Span<const ScenarioDiplomacyOverride> diplomacyOverrides() const noexcept {
        return m_diplomacyOverrides;
    }

    // Resolves player/team aliases, assigns canonical ScriptTeam IDs and
    // freezes deterministic lookup state.  Unknown team owners are reported
    // rather than implicitly becoming an arbitrary live player.
    [[nodiscard]] bool finalize(const ResolvedMatchSetup& setup,
                                container::Vector<ScenarioValidationIssue>* issues = nullptr);

    [[nodiscard]] OwnerReference resolveOwner(container::StringView alias) const;
    // ScriptList ownership is position-based in RefCode.  This lookup never
    // falls back to a name, so duplicate legacy player names cannot redirect
    // `ThisPlayer` effects to the wrong Side.
    [[nodiscard]] std::optional<PlayerId> playerForSourceSide(uint32_t sourceSideOrdinal) const noexcept;
    [[nodiscard]] const ScenarioPlayerBinding* findPlayer(PlayerId player) const noexcept;
    [[nodiscard]] const ScriptTeamDefinition* findScriptTeam(ScriptTeamId id) const noexcept;
    [[nodiscard]] bool isFinalized() const noexcept { return m_finalized; }
private:
    struct OwnerAlias final {
        container::String canonicalName;
        OwnerReference reference;
    };

    [[nodiscard]] OwnerReference lookupOwner(container::StringView alias) const;
    container::Vector<ScenarioPlayerBinding> m_players;
    container::Vector<ScenarioSideBinding> m_sides;
    container::Vector<ScriptTeamDefinition> m_scriptTeams;
    container::Vector<ScenarioBuildIntent> m_buildIntents;
    container::Vector<ScenarioDiplomacyOverride> m_diplomacyOverrides;
    container::Vector<OwnerAlias> m_ownerAliases;
    bool m_finalized = false;
};

} // namespace engine::scenario

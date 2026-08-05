#pragma once

#include "core/container/hash_containers.h"

#include "container/ordered_id_set.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectRelationshipPolicy.h"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine {

// Runtime kinds deliberately model ownership, not UI/selection groups.  A
// player default team is where ordinary production lands; a Scenario record
// is one live instance of a CkMp SidesList Team prototype.  The immutable
// ScenarioDefinition owns prototype data and aliases; this registry owns only
// materialized instances and their ObjectId membership.
enum class ObjectTeamKind : uint8_t {
    PlayerDefault,
    Scenario,
};

enum class ObjectTeamAssemblyKind : uint8_t {
    None,
    Recruit,
    Production,
    ProductionReady,
};

struct ObjectTeamProductionWorkOrder final {
    ObjectId producer = INVALID_OBJECT_ID;
    uint64_t nextAttemptTick = 0;
    uint32_t failureCount = 0;
};

struct ObjectTeamRecord final {
    ObjectTeamId id = INVALID_OBJECT_TEAM_ID;
    ObjectTeamKind kind = ObjectTeamKind::PlayerDefault;
    PlayerId owner = INVALID_PLAYER_ID;
    // TeamFactory keeps singleton instances inactive until setActive().  A
    // non-singleton can also be created inactive while map import populates
    // it.  `active` is therefore distinct from this record existing.
    bool active = true;
    // A Team creation is a one-confirmed-tick event in legacy script
    // conditions. Map-import materialization does not fabricate this pulse;
    // an inactive -> active transition records one. Player default Teams are
    // activated during legacy newGame, so their startup pulse is deferred to
    // the first confirmed ScriptEngine frame below.
    bool hasCreationPulse = false;
    bool pendingInitialCreationPulse = false;
    uint64_t createdAtConfirmedTick = 0;
    // BUILD_TEAM creates an inactive Team and immediately executes the
    // authored production-condition THEN actions; RefCode may request the
    // same actions again while an assembling member is idle. This pulse is
    // distinct from the later active Team-created/onCreate edge.
    bool hasProductionStartPulse = false;
    bool pendingProductionStartPulse = false;
    uint64_t productionStartedAtConfirmedTick = 0;
    uint32_t productionActionPulseCount = 0;
    uint32_t pendingProductionActionPulseCount = 0;
    uint32_t productionActionWithoutTeamPulseCount = 0;
    uint32_t pendingProductionActionWithoutTeamPulseCount = 0;
    // Present for each runtime instance created from a map's immutable
    // ScriptTeam definition. PlayerDefault records may be bound to such a
    // definition as an alias without changing this field.
    ScriptTeamId scenarioDefinition = INVALID_SCRIPT_TEAM_ID;
    container::String name;
    // Team::m_state in RefCode is a script-authored opaque label. It is not
    // an Object AI state and must not be projected from member state machines.
    container::String scriptState;
    // RefCode stores `m_isRecruitablitySet` plus `m_isRecruitable`. An
    // optional value expresses the same three states without parallel booleans:
    // no override, forced recruitable, forced non-recruitable.
    std::optional<bool> recruitableOverride;
    // Mutable AI production priority used by TEAM_INCREASE/DECREASE_PRIORITY.
    // It is intentionally stored on the live Team instance so future
    // production planners can consume the confirmed value directly.
    int32_t productionPriority = 1;
    ObjectId commonTarget = INVALID_OBJECT_ID;
    // AutoReinforce products join the active Team immediately for ownership,
    // but defer joinTeam-style order inheritance until their factory exit
    // route reaches Idle. ObjectId order keeps the pending set deterministic.
    container::Vector<ObjectId> pendingReinforcements;
    ObjectTeamAssemblyKind assemblyKind = ObjectTeamAssemblyKind::None;
    uint64_t assemblyDeadlineTick = 0;
    uint64_t assemblyStartedTick = 0;
    bool assemblyStartTickKnown = false;
    uint32_t assemblySourceSequence = 0;
    // Script BUILD_TEAM is a priority build and may queue behind a busy
    // compatible factory. Autonomous AI selection is low priority and waits
    // for an idle factory, matching AIPlayer::buildSpecificAITeam.
    bool productionMayUseBusyFactory = false;
    // BUILD_TEAM WorkOrder completion is historical: losing or transferring
    // a completed member must not cause the factory planner to replace it.
    // Entries retain ScenarioTeamPlan::units author order.
    container::Vector<uint32_t> productionCompletedByUnit;
    container::Vector<ObjectTeamProductionWorkOrder>
        productionWorkOrders;
    uint64_t policyRevision = 1;
    container::SharedPtr<const ObjectRelationshipOverridePolicy>
        relationshipPolicy;
    // Modern aggregate queries use the ObjectId-sorted set below. RefCode's
    // intrusive TeamMemberList, however, prepends every assignment, making
    // newest-first order observable to a small set of script actions (for
    // example getEstimateTeamPosition() choosing the first member). Keep that
    // compatibility order as stable IDs instead of reintroducing pointers or
    // coupling ScriptRuntime to ECS storage.
    container::Vector<ObjectId> legacyMemberOrder;
    container::OrderedIdSet<ObjectId> members;
};

struct ObjectTeamScenarioBindingSnapshot final {
    ScriptTeamId definition = INVALID_SCRIPT_TEAM_ID;
    container::Vector<ObjectTeamId> instances;
    int32_t productionPriority = 1;
    int32_t productionPrioritySuccessIncrease = 1;
    int32_t productionPriorityFailureDecrease = 1;
    bool productionPolicyConfigured = false;
    container::String attackPrioritySet;
};

struct ObjectTeamRegistrySnapshot final {
    static constexpr uint32_t SchemaVersion = 4;

    uint32_t schemaVersion = SchemaVersion;
    container::Vector<ObjectTeamRecord> teams;
    container::Array<ObjectTeamId, PLAYER_REGISTRY_CAPACITY> defaultTeams{};
    container::Vector<ObjectTeamScenarioBindingSnapshot> scenarioTeams;
};

// Match-owned primary-team registry.  It stores stable ObjectIds only, so
// team/AI/script consumers never retain an EnTT entity. The unordered map is
// reverse lookup only. `members()` is the modern deterministic aggregate
// view; `legacyMembers()` is an explicit compatibility view for RefCode paths
// whose result depends on the prepended intrusive-list order.
class ObjectTeamRegistry final {
public:
    void reset() noexcept;

    // Must be called once after PlayerRegistry has materialized the complete
    // match/scenario roster.  `players` is already PlayerId-sorted.
    [[nodiscard]] bool initializePlayerDefaults(container::Span<const PlayerId> players);
    // ScriptEngine clears Team::m_created only after it has evaluated the
    // frame's scripts. Player::setDefaultTeam() happened during newGame, so
    // expose its one legacy pulse on the first confirmed frame rather than at
    // session construction time (which may not have a tick yet).
    void beginConfirmedTick(uint64_t confirmedTick) noexcept;
    // Bind a SidesList player-default prototype (`team<playerName>`) to the
    // already-live PlayerDefault record.  It does not create another Team.
    [[nodiscard]] bool bindScenarioTeamAlias(ScriptTeamId definition, ObjectTeamId team);
    [[nodiscard]] bool configureScenarioTeamProductionPolicy(
        ScriptTeamId definition, int32_t priority,
        int32_t successIncrease, int32_t failureDecrease);
    // Materialize one instance of an immutable Scenario Team prototype.  It
    // may be inactive (map import / singleton initialization) or immediately
    // active (an explicit create operation). Multiple non-singleton instances
    // are retained under the same definition in creation order.
    [[nodiscard]] std::optional<ObjectTeamId> createScenarioTeamInstance(
        ScriptTeamId definition, container::String name, PlayerId owner, bool active,
        uint64_t createdAtConfirmedTick = 0);
    // Compatibility spelling for direct callers/tests that create an active
    // Scenario Team instance and expect a TeamCreated pulse at the supplied
    // confirmed tick.
    [[nodiscard]] std::optional<ObjectTeamId> createScenarioTeam(
        ScriptTeamId definition, container::String name, PlayerId owner,
        uint64_t createdAtConfirmedTick = 0);

    [[nodiscard]] std::optional<ObjectTeamId> defaultTeam(PlayerId owner) const noexcept;
    // Returns the first instance of this Scenario prototype, matching the
    // legacy name-only lookup. It deliberately includes inactive instances.
    [[nodiscard]] std::optional<ObjectTeamId> scenarioTeam(ScriptTeamId definition) const noexcept;
    [[nodiscard]] container::Span<const ObjectTeamId> scenarioTeamInstances(
        ScriptTeamId definition) const noexcept;
    [[nodiscard]] std::optional<ObjectTeamId> teamOf(ObjectId object) const noexcept;
    [[nodiscard]] std::optional<PlayerId> teamOwner(ObjectTeamId team) const noexcept;
    [[nodiscard]] std::optional<container::StringView> scriptState(
        ObjectTeamId team) const noexcept;
    [[nodiscard]] ObjectId commonTarget(ObjectTeamId team) const noexcept;
    [[nodiscard]] const ObjectTeamRecord* find(ObjectTeamId team) const noexcept;
    [[nodiscard]] bool isOwnedBy(ObjectTeamId team, PlayerId owner) const noexcept;
    [[nodiscard]] bool isActive(ObjectTeamId team) const noexcept;
    // Map loading finishes before any confirmed ScriptEngine frame exists.
    // RefCode activates a Team immediately after its first successfully
    // constructed map Object, but scripts observe `m_created` only on the
    // following first frame. Preserve that timing without inventing tick 0.
    [[nodiscard]] bool activateAtStartup(ObjectTeamId team) noexcept;
    // `setActive()` is idempotent in RefCode. A second activation therefore
    // never emits a second TEAM_CREATED pulse.
    [[nodiscard]] bool activate(ObjectTeamId team, uint64_t confirmedTick) noexcept;
    // Script Team deletion keeps the stable record/ID but makes name lookup
    // and creation-state queries observe an inactive, empty instance.
    [[nodiscard]] bool deactivate(ObjectTeamId team) noexcept;
    [[nodiscard]] bool beginAssembly(
        ObjectTeamId team, ObjectTeamAssemblyKind kind,
        uint64_t deadlineTick, uint32_t sourceSequence,
        std::optional<uint64_t> startedTick = std::nullopt,
        bool productionMayUseBusyFactory = false) noexcept;
    [[nodiscard]] bool clearAssembly(ObjectTeamId team) noexcept;
    [[nodiscard]] bool updateAssemblySourceSequence(
        ObjectTeamId team, uint32_t sourceSequence) noexcept;
    [[nodiscard]] bool initializeProductionProgress(
        ObjectTeamId team, size_t rosterEntryCount);
    [[nodiscard]] bool recordProductionUnitCompleted(
        ObjectTeamId team, uint32_t rosterIndex) noexcept;
    [[nodiscard]] uint32_t productionUnitCompleted(
        ObjectTeamId team, uint32_t rosterIndex) const noexcept;
    [[nodiscard]] ObjectTeamProductionWorkOrder productionWorkOrder(
        ObjectTeamId team, uint32_t rosterIndex) const noexcept;
    [[nodiscard]] bool updateProductionWorkOrder(
        ObjectTeamId team, uint32_t rosterIndex, ObjectId producer,
        uint64_t nextAttemptTick, bool failed) noexcept;
    [[nodiscard]] bool markProductionStarted(
        ObjectTeamId team, uint64_t confirmedTick,
        uint32_t actionCount = 1,
        bool bindTeamContext = true) noexcept;
    // Retires an empty non-singleton Scenario instance without reusing its
    // stable ID. The slot remains as an invalid tombstone; definition lookup
    // and hook iteration no longer expose it.
    [[nodiscard]] bool retireEmptyScenarioTeam(ObjectTeamId team) noexcept;
    [[nodiscard]] bool wasCreatedAt(ObjectTeamId team, uint64_t confirmedTick) const noexcept;
    [[nodiscard]] uint32_t productionActionCountAt(
        ObjectTeamId team, uint64_t confirmedTick) const noexcept;
    [[nodiscard]] uint32_t productionActionWithoutTeamCountAt(
        ObjectTeamId team, uint64_t confirmedTick) const noexcept;
    [[nodiscard]] container::Span<const ObjectId> members(ObjectTeamId team) const noexcept;
    [[nodiscard]] uint64_t membershipRevision(
        ObjectTeamId team) const noexcept;
    [[nodiscard]] container::Span<const ObjectId> legacyMembers(
        ObjectTeamId team) const noexcept;
    [[nodiscard]] container::Span<const ObjectTeamRecord> teams() const noexcept { return m_teams; }
    [[nodiscard]] bool captureSnapshot(
        ObjectTeamRegistrySnapshot& output) const;
    [[nodiscard]] bool restoreSnapshot(
        const ObjectTeamRegistrySnapshot& snapshot);
    [[nodiscard]] uint64_t stableHash() const noexcept;

    // These mutate value-only indexes only. GameSession is the sole public
    // authority that pairs them with OwnerComponent/ObjectLifecycle changes.
    [[nodiscard]] bool assignObject(ObjectTeamId team, ObjectId object);
    void removeObject(ObjectId object) noexcept;
    [[nodiscard]] bool setTeamOwner(ObjectTeamId team, PlayerId owner) noexcept;
    [[nodiscard]] bool setScriptState(ObjectTeamId team,
                                      container::String state);
    [[nodiscard]] bool setCommonTarget(
        ObjectTeamId team, ObjectId target) noexcept;
    [[nodiscard]] bool clearCommonTargetIf(
        ObjectTeamId team, ObjectId target) noexcept;
    [[nodiscard]] bool addPendingReinforcement(
        ObjectTeamId team, ObjectId object);
    [[nodiscard]] bool removePendingReinforcement(
        ObjectTeamId team, ObjectId object) noexcept;
    [[nodiscard]] bool setRecruitableOverride(ObjectTeamId team,
                                               bool recruitable) noexcept;
    [[nodiscard]] bool adjustProductionPriority(
        ObjectTeamId team, int32_t delta) noexcept;
    [[nodiscard]] std::optional<int32_t> productionPriority(
        ObjectTeamId team) const noexcept;
    [[nodiscard]] bool setAttackPrioritySet(
        ObjectTeamId team, container::String setName);
    [[nodiscard]] std::optional<container::StringView> attackPrioritySet(
        ObjectTeamId team) const noexcept;
    [[nodiscard]] bool setTeamRelationshipOverride(
        ObjectTeamId source, ObjectTeamId target,
        PlayerRelationship relationship);
    [[nodiscard]] bool removeTeamRelationshipOverride(
        ObjectTeamId source, ObjectTeamId target);
    [[nodiscard]] bool setPlayerRelationshipOverride(
        ObjectTeamId source, PlayerId target,
        PlayerRelationship relationship);
    [[nodiscard]] bool removePlayerRelationshipOverride(
        ObjectTeamId source, PlayerId target);
    [[nodiscard]] bool clearRelationshipOverrides(ObjectTeamId source);
    [[nodiscard]] container::SharedPtr<const ObjectRelationshipOverridePolicy>
    relationshipPolicy(ObjectTeamId team) const noexcept;

private:
    struct ScenarioTeamBinding final {
        ScriptTeamId definition = INVALID_SCRIPT_TEAM_ID;
        // Creation order is observable through legacy first-instance lookup.
        // The first entry may be a PlayerDefault alias rather than a Scenario
        // record; all entries remain stable ObjectTeamIds.
        container::Vector<ObjectTeamId> instances;
        int32_t productionPriority = 1;
        int32_t productionPrioritySuccessIncrease = 1;
        int32_t productionPriorityFailureDecrease = 1;
        bool productionPolicyConfigured = false;
        // Team::setAttackPriorityName writes TeamPrototype, so every live
        // instance and future member of this authored prototype shares it.
        container::String attackPrioritySet;
    };

    [[nodiscard]] ObjectTeamRecord* mutableFind(ObjectTeamId team) noexcept;
    [[nodiscard]] std::optional<ObjectTeamId> appendTeam(ObjectTeamKind kind, PlayerId owner,
                                                           ScriptTeamId definition, container::String name,
                                                           bool active,
                                                           uint64_t createdAtConfirmedTick,
                                                           bool hasCreationPulse,
                                                           bool pendingInitialCreationPulse);
    // `findScenarioTeam()` is an exact lookup. Keep lower-bound insertion
    // separate: treating a later binding as a hit would silently redirect a
    // Team query or append an instance to the wrong immutable prototype.
    [[nodiscard]] container::Vector<ScenarioTeamBinding>::iterator lowerBoundScenarioTeam(
        ScriptTeamId definition);
    [[nodiscard]] container::Vector<ScenarioTeamBinding>::const_iterator lowerBoundScenarioTeam(
        ScriptTeamId definition) const;
    [[nodiscard]] container::Vector<ScenarioTeamBinding>::iterator findScenarioTeam(ScriptTeamId definition);
    [[nodiscard]] container::Vector<ScenarioTeamBinding>::const_iterator findScenarioTeam(
        ScriptTeamId definition) const;

    container::Vector<ObjectTeamRecord> m_teams;
    container::Array<ObjectTeamId, PLAYER_REGISTRY_CAPACITY> m_defaultTeams{};
    container::Vector<ScenarioTeamBinding> m_scenarioTeams;
    container::HashMap<ObjectId, ObjectTeamId> m_teamByObject;
};

} // namespace engine

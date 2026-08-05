#pragma once

#include "game/object/component/ObjectComponentsMovementPhysics.h"

#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

namespace engine {

struct OwnerComponent {
    PlayerId player = INVALID_PLAYER_ID;
};

// Per-object command-set override installed by CommandSetUpgrade. Empty is
// intentionally meaningful: Object::getCommandSetString falls back to the
// immutable template CommandSet when its override string is empty. Revision
// advances on every authored application (including a mux reset/regrant that
// selects the same string), replacing the old global ControlBar dirty flag
// with a narrow value-state change boundary.
struct ObjectCommandSetOverrideComponent final {
    ::container::String name;
    uint64_t revision = 0;
    uint64_t lastAppliedTick = 0;
};

// Persistent renderer-neutral equivalent of W3DDrawModule's upgraded
// subobject table. Every potential name is materialized before publication;
// UpgradeMux execution only toggles active/value bits, avoiding allocations
// in a confirmed upgrade transaction. Render extraction appends active entries
// after ConditionState/animation visibility so these overrides win exactly as
// SubObjectsUpgrade promises.
struct ObjectSubObjectVisibilityOverride final {
    ::container::String name;
    bool visible = true;
    bool active = false;
};

struct ObjectSubObjectVisibilityOverrideComponent final {
    ::container::Vector<ObjectSubObjectVisibilityOverride> entries;
    uint64_t revision = 0;
};

// Immutable template energy values are copied into a compact per-entity
// component so the player aggregate never looks through ThingFactory or an
// old Object pointer. A positive base contribution produces power; a negative
// one consumes it, exactly matching RefCode Energy::objectEnteringInfluence.
//
// RefCode has two independent producers of EnergyBonus: PowerPlantUpgrade and
// OverchargeBehavior.  They can overlap, so this must not be a lone boolean.
// The typed source vocabulary also gives a later confirmed upgrade/ability
// transaction a narrow, value-only mutation surface rather than making it
// infer state from legacy module names.
enum class ObjectEnergyBonusSource : uint8_t {
    PowerPlantUpgrade,
    Overcharge,
    Count,
};

using ObjectEnergyBonusSourceMask = uint8_t;

[[nodiscard]] constexpr ObjectEnergyBonusSourceMask
objectEnergyBonusSourceBit(ObjectEnergyBonusSource source) noexcept {
    return source >= ObjectEnergyBonusSource::Count
        ? ObjectEnergyBonusSourceMask{}
        : static_cast<ObjectEnergyBonusSourceMask>(
              ObjectEnergyBonusSourceMask{1} << static_cast<uint8_t>(source));
}

struct ObjectEnergyComponent final {
    int32_t baseContribution = 0;
    int32_t bonusProduction = 0;
    ObjectEnergyBonusSourceMask bonusProductionSources = 0;
};

// A value-only edge from an object currently stored inside a transport,
// garrison, tunnel, or other future Contain implementation to its host.
// Container systems alone create/clear this component; consumers such as
// HeightDieUpdate merely observe it and never follow an ECS entity pointer.
struct ObjectContainedByComponent final {
    ObjectId container = INVALID_OBJECT_ID;
    // Exact immutable containment rule selected when this edge was admitted.
    // A host can legally own several Contain occurrences whose KindOf masks
    // overlap; re-selecting by passenger kind during exit can therefore apply
    // another module's ExitDelay/door/velocity policy.  UINT32_MAX is reserved
    // for structural forced edges (OCL add-ons/slaves) that did not enter via
    // the authored containment admission path.
    uint32_t containmentRuleIndex = std::numeric_limits<uint32_t>::max();
    // Confirmed logic tick at which this exact edge was committed. Timed
    // containment modules (notably HealContain) own their clock here instead
    // of attempting to infer elapsed time from mutable passenger state.
    uint64_t confirmedEnteredTick = 0;
    // OCL-created upgrades (Overlord/Helix add-ons) are structural children:
    // the host's terminal lifecycle owns them. Ordinary transport passengers
    // can use the same edge with this bit clear and choose eject/kill policy
    // in their dedicated Contain module.
    bool destroyWithContainer = false;
    // RefCode's ContainedBy pointer does not imply that the object has left
    // the world.  Station garrisons and Overlord/Helix portable structures
    // remain spatial, targetable objects even though their transform and
    // structural lifetime are owned by a host.  Only an enclosing edge is
    // removed from the spatial/targeting world and receives DISABLED_HELD.
    bool enclosing = true;
    bool followsContainerTransform = true;
    bool hadMapStatus = false;
    bool previousOffMap = false;
};

struct ObjectContainedObjectRecord final {
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t confirmedEnteredTick = 0;
    // Stable per-container admission order. Storage remains ObjectId-sorted
    // for deterministic lookup, while authored FIFO consumers such as
    // DeliverPayload can recover ZH's ContainedItemsList front semantics.
    uint64_t entryOrdinal = 0;
    bool destroyWithContainer = false;
};

// Sparse reverse edge owned by a live container. Records remain sorted by
// stable ObjectId; no consumer relies on EnTT storage order or scans every
// object to discover a host's occupants.
struct ObjectContainmentComponent final {
    ::container::Vector<ObjectContainedObjectRecord> objects;
    uint64_t nextEntryOrdinal = 1;
    // Runtime override owned by PassengersFireUpgrade. The permission lives
    // on the host even while it is empty, so passengers entering later see
    // the already-completed upgrade without replaying an UpgradeMux.
    bool passengersAllowedToFire = false;
    uint64_t revision = 0;
};

// Player EVACUATE on an airborne transport first owns a move/landing order,
// then resumes normal per-passenger Exit after landing. externalOrderRevision
// cancels the continuation when any later explicit command overrides it.
struct ObjectPendingPlayerEvacuationComponent final {
    PlayerId player = INVALID_PLAYER_ID;
    uint64_t externalOrderRevision = 0;
    uint64_t issuedTick = 0;
    uint64_t deadlineTick = 0;
    uint32_t sourceSequence = 0;
    math::q32_32 landingZ{};
    bool previousUsePreciseZPosition = false;
};

// Stable, value-only FIREPOINT ownership for an enclosing GarrisonContain.
// RefCode keeps this as mutable point data on the module and re-shuffles an
// occupant only when a closer free point exists for its current victim.  The
// modern projection stores the same facts by ObjectId so combat can preserve
// a point across ticks without retaining ECS entities, Drawable pointers, or
// renderer state.  A non-object target is represented by INVALID_OBJECT_ID
// plus its fixed-point position.
struct ObjectGarrisonFirePointAssignment final {
    ObjectId occupant = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    // Confirmed world-space FIREPOINT selected by GarrisonContain.  The
    // enclosing passenger keeps its authoritative transform at the
    // container centre; presentation therefore cannot reconstruct this
    // point from the hidden passenger later without repeating simulation
    // bone selection.
    LogicFixedVec3 pointPosition{};
    LogicFixedVec3 targetPosition{};
    uint64_t lastEffectFireTick = 0;
    uint32_t lastEffectFireSequence = 0;
    uint16_t pointIndex = 0;
    bool suppressMuzzleFlash = false;
};

struct ObjectGarrisonFirePointComponent final {
    ::container::Vector<ObjectGarrisonFirePointAssignment> assignments;
    uint64_t revision = 0;
};

// Every normal game object belongs to one live ObjectTeam.  The Team is the
// authoritative grouping used by scripts/AI/production; OwnerComponent is
// its controlling Player projection.  Keeping both values explicit avoids
// the original pointer chain while making an inconsistent transfer detectable.
struct PrimaryTeamComponent {
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
};

// Preserve the original assignment once, before a future temporary capture /
// restore system changes PrimaryTeamComponent.  The current lifecycle only
// records this value; behavior modules will choose when to restore it.
struct OriginalOwnershipComponent {
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
};

struct ThingTemplateComponent {
    ::container::String name;
    // The session freezes the compiled archetype the first time it is spawned
    // and shares that immutable copy with every matching entity. This must
    // never point directly into ThingFactory: VFS/content reloads may clear
    // that store while a session renderer still extracts a completed frame.
    ::container::SharedPtr<const game::ObjectArchetype> archetype;
};

// Mutable selector state paired with an immutable Archetype-owned
// CombatProfile.  It replaces RefCode's per-object ArmorSet/WeaponSet
// pointer chains with two compact masks; cooldown, ammo and targeting state
// belong to the later CombatSystem rather than this content projection.
struct ObjectCombatProfileComponent final {
    ::container::SharedPtr<const game::CombatProfile> profile;
    game::WeaponSetConditionMask weaponConditions = 0;
    game::ArmorSetConditionMask armorConditions = 0;
};

// Independent weapon-scalar conditions.  This must never be folded into
// ObjectCombatProfileComponent::weaponConditions: WeaponSet selection and
// WeaponBonus arithmetic are separate legacy domains that happen to contain
// similarly named PLAYER_UPGRADE entries. Revision gives future presentation
// and checksum code a compact change fact without watching every weapon slot.
struct ObjectWeaponBonusComponent final {
    game::WeaponBonusConditionMask conditions = 0;
    uint64_t revision = 0;
    uint64_t lastChangedTick = 0;
};

// Per-object application ledger for OBJECT_ALLOW_BONUSES. Keeping the exact
// health multiplier which was applied makes a later disable or ownership
// transfer an inverse fixed-point transaction even if the new owner uses a
// different difficulty row.
struct ObjectDifficultyBonusComponent final {
    math::q32_32 appliedHealthMultiplier{int32_t{1}};
    game::WeaponBonusCondition appliedWeaponCondition =
        game::WeaponBonusCondition::Count;
    uint64_t revision = 1;
    bool receiving = false;
};

// Enter/Garrison actions are asynchronous AI intents in RefCode. Both player
// contextual entry and authored script actions use this legacy-named value
// component. It keeps only stable IDs and the stamped system-order identity;
// ordinary movement owns locomotion, and ObjectContainmentSystem remains the
// sole authority which can commit the final attachment.
struct ObjectScriptContainmentEnterComponent final {
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t issuedTick = 0;
    uint32_t sourceSequence = 0;
    // Capacity reserved while locomotion is still approaching the host.
    // Garrison entries reserve one occupant; transport loading supplies the
    // passenger's authored TransportSlotCount. It is value-only and is
    // released automatically when this sparse intent is removed/replaced.
    uint32_t reservedCapacity = 1;
    // A direct host approach may complete at a collision boundary before the
    // attach-radius test observes the live target transform. Retrying is
    // deterministic and bounded; this is not an open-ended path request.
    uint8_t approachAttempts = 0;
    uint64_t revision = 1;
};

// Canonical Zero Hour KindOf mask copied from the immutable archetype.
struct ObjectKindOfComponent final {
    game::ObjectKindOfMask mask{};
};

// Per-object AI policy which can be changed by legacy map scripts without
// coupling the typed ScriptRuntime to ObjectAIRuntime storage.  RefCode keeps
// both values on AIUpdateInterface: AttackPriorityInfo selects autonomous
// victims and Attitude drives the mood/command admission matrix. Authored
// strings stop at the session catalog boundary; hot target scans use a compact
// ID.
enum class ObjectAIAttitude : int8_t {
    Sleep = -2,
    Passive = -1,
    Normal = 0,
    Alert = 1,
    Aggressive = 2,
};

struct ObjectAIBehaviorPolicyComponent final {
    // Zero selects the unnamed default. Non-zero IDs are session-local,
    // monotonic handles into GameSession's immutable-address catalog nodes;
    // confirmed target scans never hash a script string per candidate.
    uint32_t attackPrioritySetId = 0;
    ObjectAIAttitude attitude = ObjectAIAttitude::Normal;
    uint64_t revision = 1;
};

// One-shot deterministic request corresponding to
// AIUpdateInterface::wakeUpAndAttemptToTarget().  Simulation producers bump
// the revision; the detached AI input boundary observes the edge and resets
// an Idle actor's target-scan deadline without exposing ObjectAIRuntime to
// containment or other gameplay systems.
struct ObjectAITargetScanWakeComponent final {
    uint64_t requestedTick = 0;
    uint64_t revision = 1;
};

} // namespace engine
